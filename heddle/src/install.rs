use crate::glob;
use crate::model::{InstallItem, InstallPlan, InstallRule, Project};
use crate::star;
use crate::sys;
use crate::toml::Toml;
use loom_core::hash::{hash_text, HASH_FNV_OFFSET};

fn join(a: &str, b: &str) -> String {
    if a.is_empty() || a == "." {
        return b.to_string();
    }
    let ab = a.as_bytes();
    let bb = b.as_bytes();
    let mut al = ab.len();
    while al > 1 && ab[al - 1] == b'/' {
        al -= 1;
    }
    let mut bi = 0usize;
    while bi < bb.len() && bb[bi] == b'/' {
        bi += 1;
    }
    format!("{}/{}", &a[..al], &b[bi..])
}

fn project_path(a: &str, b: &str) -> String {
    if a.is_empty() || a == "." {
        return b.to_string();
    }
    format!("{}/{}", a, b)
}

fn strip_root(p: &str) -> &str {
    let b = p.as_bytes();
    let mut i = 0usize;
    while i < b.len() && b[i] == b'/' {
        i += 1;
    }
    if i + 1 < b.len() && b[i + 1] == b':' {
        i += 2;
    }
    while i < b.len() && (b[i] == b'/' || b[i] == b'\\') {
        i += 1;
    }
    &p[i..]
}

fn dest_join(destdir: Option<&str>, prefix: &str, sub: &str, leaf: &str) -> String {
    let dd = match destdir {
        Some(d) if !d.is_empty() => Some(d),
        _ => None,
    };
    let pfx = if dd.is_some() { strip_root(prefix) } else { prefix };
    let root = join(dd.unwrap_or(""), pfx);
    join(&join(&root, sub), leaf)
}

fn dest_join_abs(destdir: Option<&str>, absroot: &str, sub: &str, leaf: &str) -> String {
    let dd = match destdir {
        Some(d) if !d.is_empty() => Some(d),
        _ => None,
    };
    let root = if dd.is_some() {
        strip_root(absroot)
    } else {
        absroot
    };
    let a = join(dd.unwrap_or(""), root);
    join(&join(&a, sub), leaf)
}

fn c_dir_of(path: &str) -> String {
    match path.rfind('/') {
        Some(i) => path[..i].to_string(),
        None => path.to_string(),
    }
}

struct ExpandItem {
    path: String,
    base: String,
}

fn glob_expand(pats: &[String]) -> Result<Vec<ExpandItem>, String> {
    let mut out: Vec<ExpandItem> = Vec::new();
    for pat in pats {
        let pb = pat.as_bytes();
        let star = pb.iter().position(|&c| c == b'*' || c == b'?');
        match star {
            Some(sp) => {
                let mut slash: Option<usize> = None;
                for (k, &c) in pb.iter().enumerate() {
                    if k >= sp {
                        break;
                    }
                    if c == b'/' {
                        slash = Some(k);
                    }
                }
                let (dir, gpat) = match slash {
                    Some(sl) => {
                        if sl >= 4096 {
                            return Err(format!("path too long: {}", pat));
                        }
                        (pat[..sl].to_string(), &pat[sl + 1..])
                    }
                    None => (".".to_string(), pat.as_str()),
                };
                let hits = glob::glob_dir(&dir, gpat);
                if hits.is_empty() {
                    return Err(format!("install pattern matches nothing: {}", pat));
                }
                for h in hits {
                    out.push(ExpandItem {
                        path: join(&dir, &h),
                        base: dir.clone(),
                    });
                }
            }
            None => out.push(ExpandItem {
                path: pat.clone(),
                base: String::new(),
            }),
        }
    }
    if out.len() > 1 {
        out.sort_by(|a, b| a.path.as_bytes().cmp(b.path.as_bytes()));
    }
    Ok(out)
}

fn add_group(
    destdir: Option<&str>,
    prefix: &str,
    root_abs: Option<&str>,
    pl: &mut InstallPlan,
    pats: &[String],
    sub: &str,
    flatten: bool,
) -> Result<(), String> {
    if pats.is_empty() {
        return Ok(());
    }
    let items = glob_expand(pats)?;
    for it in &items {
        if sys::stat(&it.path).is_none() {
            return Err(format!("install source missing: {}", it.path));
        }
        let leaf = if flatten {
            sys::base_name(&it.path).to_string()
        } else {
            let rel = if !it.base.is_empty() && it.path.starts_with(&it.base) {
                let mut r = &it.path[it.base.len()..];
                while r.starts_with('/') {
                    r = &r[1..];
                }
                r
            } else {
                it.path.as_str()
            };
            if rel.is_empty() {
                sys::base_name(&it.path).to_string()
            } else {
                rel.to_string()
            }
        };
        let dst = match root_abs {
            Some(ra) => dest_join_abs(destdir, ra, sub, &leaf),
            None => dest_join(destdir, prefix, sub, &leaf),
        };
        pl.items.push(InstallItem {
            src: it.path.clone(),
            dst,
        });
    }
    Ok(())
}

fn add_tree(
    pl: &mut InstallPlan,
    src: &str,
    dstroot: &str,
    rel: &str,
) -> Result<(), String> {
    let rd = match std::fs::read_dir(src) {
        Ok(r) => r,
        Err(_) => return Err(format!("cannot read {}", src)),
    };
    for e in rd {
        let e = match e {
            Ok(e) => e,
            Err(_) => continue,
        };
        let name = e.file_name().to_string_lossy().into_owned();
        if name == "." || name == ".." {
            continue;
        }
        let full = join(src, &name);
        let rel2 = if rel.is_empty() {
            name.clone()
        } else {
            format!("{}/{}", rel, name)
        };
        if sys::is_dir(&full) {
            add_tree(pl, &full, dstroot, &rel2)?;
        } else {
            pl.items.push(InstallItem {
                src: full,
                dst: join(dstroot, &rel2),
            });
        }
    }
    Ok(())
}

fn load_key(t: &Toml, sect: &str, key: &str, root: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    if let Some(one) = t.str_of(sect, key) {
        out.push(join(root, one));
    }
    let mut i = 0usize;
    while let Some(v) = t.arr(sect, key, i) {
        out.push(join(root, v));
        i += 1;
    }
    out
}

fn dup_list(src: &[String], root: &str) -> Vec<String> {
    src.iter().map(|s| project_path(root, s)).collect()
}

fn load_star(p: &Project, path: &str) -> Result<Vec<InstallRule>, String> {
    let cfg = star::run(path)?;
    let mut rules: Vec<InstallRule> = Vec::new();
    for t in &cfg.targets {
        if t.i_bin.is_empty()
            && t.i_lib.is_empty()
            && t.i_include.is_empty()
            && t.i_share.is_empty()
            && t.i_etc.is_empty()
            && t.i_rootfs.is_none()
        {
            continue;
        }
        rules.push(InstallRule {
            name: t.name.clone(),
            bin: dup_list(&t.i_bin, &p.root),
            lib: dup_list(&t.i_lib, &p.root),
            include: dup_list(&t.i_include, &p.root),
            share: dup_list(&t.i_share, &p.root),
            etc: dup_list(&t.i_etc, &p.root),
            sysroot_lib: Vec::new(),
            sysroot_include: Vec::new(),
            rootfs: t.i_rootfs.clone(),
        });
    }
    Ok(rules)
}

pub fn load(p: &Project) -> Result<Vec<InstallRule>, String> {
    let cfg = format!("{}/heddle.toml", p.root);
    let star = format!("{}/heddle.star", p.root);
    if sys::stat(&star).is_some() {
        return load_star(p, &star);
    }

    let t = Toml::parse(&cfg)?;
    let mut rules: Vec<InstallRule> = Vec::new();
    for name in t.sections("target.") {
        let sect = format!("target.{}.install", name);
        let rootfs = t.str_of(&sect, "rootfs").map(|v| v.to_string());
        let mut has = rootfs.is_some();
        if !has {
            let keys = [
                "bin",
                "lib",
                "include",
                "share",
                "etc",
                "sysroot_lib",
                "sysroot_include",
            ];
            for k in keys {
                if t.str_of(&sect, k).is_some() || t.arr(&sect, k, 0).is_some() {
                    has = true;
                }
            }
        }
        if !has {
            continue;
        }
        rules.push(InstallRule {
            name: name.clone(),
            bin: load_key(&t, &sect, "bin", &p.root),
            lib: load_key(&t, &sect, "lib", &p.root),
            include: load_key(&t, &sect, "include", &p.root),
            share: load_key(&t, &sect, "share", &p.root),
            etc: load_key(&t, &sect, "etc", &p.root),
            sysroot_lib: load_key(&t, &sect, "sysroot_lib", &p.root),
            sysroot_include: load_key(&t, &sect, "sysroot_include", &p.root),
            rootfs,
        });
    }
    Ok(rules)
}

pub fn has_any(rules: &[InstallRule]) -> bool {
    !rules.is_empty()
}

pub fn plan(
    rules: &[InstallRule],
    p: &mut Project,
    target: Option<&str>,
    prefix: &str,
    destdir: Option<&str>,
) -> Result<InstallPlan, String> {
    let mut pl = InstallPlan::default();
    for r in rules {
        if let Some(t) = target {
            if t != r.name {
                continue;
            }
        }

        add_group(destdir, prefix, None, &mut pl, &r.bin, "bin", true)?;
        add_group(destdir, prefix, None, &mut pl, &r.lib, "lib", true)?;
        add_group(destdir, prefix, None, &mut pl, &r.include, "include", false)?;

        let sub_share = format!("share/{}", r.name);
        add_group(destdir, prefix, None, &mut pl, &r.share, &sub_share, true)?;

        let sub_etc = format!("etc/{}", r.name);
        add_group(destdir, prefix, None, &mut pl, &r.etc, &sub_etc, true)?;

        let sysroot = match &p.pkg.sysroot {
            Some(s) if !s.is_empty() => Some(s.clone()),
            _ => p.target_sysroot.clone(),
        };
        let sysroot_v = match &sysroot {
            Some(s) if !s.is_empty() => Some(s.as_str()),
            _ => None,
        };
        if (!r.sysroot_lib.is_empty() || !r.sysroot_include.is_empty()) && sysroot_v.is_none() {
            return Err(format!(
                "target '{}': sysroot_* install needs [target] sysroot",
                r.name
            ));
        }

        if !r.sysroot_lib.is_empty() {
            add_group(
                destdir,
                "",
                sysroot_v,
                &mut pl,
                &r.sysroot_lib,
                "lib",
                true,
            )?;
        }
        if !r.sysroot_include.is_empty() {
            add_group(
                destdir,
                "",
                sysroot_v,
                &mut pl,
                &r.sysroot_include,
                "include",
                false,
            )?;
        }

        if let Some(rootfs) = &r.rootfs {
            let src = join(&p.root, rootfs);
            let dstroot = dest_join_abs(destdir, prefix, "rootfs", "");
            add_tree(&mut pl, &src, &dstroot, "")?;
        }
    }
    Ok(pl)
}

pub fn run(pl: &InstallPlan, log: &str, verbose: bool) -> Result<(), String> {
    for it in &pl.items {
        let dir = c_dir_of(&it.dst);
        sys::mkpath(&dir);
        if !copy_file(&it.src, &it.dst) {
            return Err(format!("cannot install {} -> {}", it.src, it.dst));
        }
        if let Some(st) = sys::stat(&it.src) {
            if st.mode & 0o111 != 0 {
                sys::chmod(&it.dst, 0o755);
            }
        }
        if verbose {
            eprintln!("heddle: install {} -> {}", it.src, it.dst);
        }
    }
    write_log(log, pl)
}

fn copy_file(src: &str, dst: &str) -> bool {
    let data = match std::fs::read(src) {
        Ok(d) => d,
        Err(_) => return false,
    };
    std::fs::write(dst, &data).is_ok()
}

pub fn write_log(log: &str, pl: &InstallPlan) -> Result<(), String> {
    let dir = c_dir_of(log);
    sys::mkpath(&dir);
    let mut out = String::new();
    out.push_str("# generated by heddle install; do not edit\n");
    for it in &pl.items {
        out.push_str(&format!("{}\t{}\n", it.dst, it.src));
    }
    match std::fs::write(log, out.as_bytes()) {
        Ok(_) => Ok(()),
        Err(_) => Err(format!("cannot write {}", log)),
    }
}

pub fn log_path(root: &str, destdir: Option<&str>, prefix: &str) -> String {
    let mut h = HASH_FNV_OFFSET;
    h = hash_text(h, destdir.unwrap_or(""));
    h = hash_text(h, prefix);
    format!("{}/.heddle/install/{:016x}.log", root, h)
}

pub fn dry_run(pl: &InstallPlan) {
    let n = pl.items.len();
    if n == 0 {
        println!("heddle: nothing to install");
        return;
    }
    println!("heddle: install plan ({} file{})", n, if n == 1 { "" } else { "s" });
    for it in &pl.items {
        println!("  {:<40} -> {}", it.src, it.dst);
    }
}

pub fn uninstall(log: &str, verbose: bool) -> Result<(), String> {
    let text = match sys::read_to_string(log) {
        Some(t) => t,
        None => return Err(format!("no install log at {}", log)),
    };
    let mut removed = 0i32;
    let mut missing = 0i32;
    for line in text.split('\n') {
        if line.starts_with('#') {
            continue;
        }
        let tab = match line.find('\t') {
            Some(i) => i,
            None => continue,
        };
        let dst = &line[..tab];
        let gone = std::fs::remove_file(dst).is_ok() || std::fs::remove_dir(dst).is_ok();
        if gone {
            removed += 1;
            if verbose {
                eprintln!("heddle: remove {}", dst);
            }
        } else {
            missing += 1;
        }
    }
    let _ = std::fs::remove_file(log);
    println!(
        "heddle: removed {} file{} ({} already gone)",
        removed,
        if removed == 1 { "" } else { "s" },
        missing
    );
    Ok(())
}
