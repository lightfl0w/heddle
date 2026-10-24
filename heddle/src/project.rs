use crate::glob;
use crate::lang;
use crate::model::{PkgKind, Project, Recipe, StarCfg, StarPlatform, StarTargetDef, Target, TargetType};
use crate::pkg;
use crate::recipe;
use crate::star;
use crate::toolchain;
use crate::sys;
use crate::toml::Toml as TomlCfg;
use std::sync::{Mutex, OnceLock};

const PKG_MAX_DEPTH: usize = 8;

fn want_target() -> &'static Mutex<Option<String>> {
    static W: OnceLock<Mutex<Option<String>>> = OnceLock::new();
    W.get_or_init(|| Mutex::new(None))
}

pub fn set_target(name: Option<&str>) {
    *want_target().lock().unwrap() = name.map(|s| s.to_string());
}

fn get_want_target() -> Option<String> {
    want_target().lock().unwrap().clone()
}

pub fn path(a: &str, b: &str) -> String {
    if a.is_empty() || a == "." {
        return b.to_string();
    }
    format!("{}/{}", a, b)
}

fn prefixed(dir: &str, rel: &str) -> String {
    if rel.starts_with('/') {
        return rel.to_string();
    }
    if dir.is_empty() {
        return rel.to_string();
    }
    path(dir, rel)
}

fn path_exists(p: &str) -> bool {
    sys::stat(p).is_some()
}

fn norm_dir(d: &str) -> String {
    let mut s = d;
    while s.starts_with("./") {
        s = &s[2..];
    }
    s.to_string()
}

fn cfg_path(dir: &str, name: &str) -> String {
    path(dir, name)
}

fn load_strings(t: &TomlCfg, section: &str, key: &str, dir: Option<&str>) -> Vec<String> {
    let mut out = Vec::new();
    for i in 0.. {
        let v = match t.arr(section, key, i) {
            Some(v) => v,
            None => break,
        };
        out.push(match dir {
            Some(d) => prefixed(d, v),
            None => v.to_string(),
        });
    }
    out
}

fn load_sources(
    t: &TomlCfg,
    section: &str,
    dir: &str,
) -> Result<Vec<String>, String> {
    let mut out: Vec<String> = Vec::new();
    for i in 0.. {
        let v = match t.arr(section, "src", i) {
            Some(v) => v,
            None => break,
        };
        if !glob::has_wild(v) {
            out.push(prefixed(dir, v));
            continue;
        }
        let base = if dir.is_empty() { "." } else { dir };
        let hits = glob::glob_dir(base, v);
        if hits.is_empty() {
            return Err(format!(
                "section [{}]: pattern '{}' matches nothing",
                section, v
            ));
        }
        for h in hits {
            out.push(prefixed(dir, &h));
        }
    }
    if out.len() > 1 {
        out.sort();
        out.dedup();
    }
    Ok(out)
}

fn target_fill(t: &mut Target, cfg: &TomlCfg, section: &str) -> Result<(), String> {
    let ty = match cfg.str_of(section, "type") {
        Some(v) => v.to_string(),
        None => return Err(format!("section [{}]: missing key 'type'", section)),
    };
    t.kind = match TargetType::parse(&ty) {
        Some(k) => k,
        None => return Err(format!("section [{}]: unknown type '{}'", section, ty)),
    };
    t.src = load_sources(cfg, section, &t.dir)?;
    t.inc = load_strings(cfg, section, "inc", Some(&t.dir));
    t.deps = load_strings(cfg, section, "deps", None);
    t.cflags = load_strings(cfg, section, "cflags", None);
    t.ldflags = load_strings(cfg, section, "ldflags", None);

    if t.kind != TargetType::Custom && t.src.is_empty() {
        return Err(format!("section [{}]: key 'src' is empty", section));
    }

    t.ldscript = cfg
        .str_of(section, "linker_script")
        .map(|v| path(&t.dir, v));
    t.entry = cfg.str_of(section, "entry").map(|v| v.to_string());
    t.format = cfg.str_of(section, "format").map(|v| v.to_string());
    t.out = cfg.str_of(section, "out").map(|v| v.to_string());
    t.start_group = cfg.bool_of(section, "start_group", false);
    t.whole_archive = cfg.bool_of(section, "whole_archive", false);

    if t.kind == TargetType::Custom {
        let cmd = cfg.str_of(section, "cmd").map(|v| v.to_string());
        match (cmd, t.out.clone()) {
            (Some(c), Some(_)) => t.cmd = Some(path(&t.dir, &c)),
            _ => return Err(format!("section [{}]: custom needs 'cmd' and 'out'", section)),
        }
    }

    if t.kind == TargetType::Raw && t.format.is_none() {
        return Err(format!(
            "section [{}]: raw needs 'format' (e.g. bin, elf)",
            section
        ));
    }
    Ok(())
}

fn add_target(p: &mut Project, name: &str, dir: &str) -> usize {
    p.targets.push(Target {
        name: name.to_string(),
        dir: dir.to_string(),
        ..Default::default()
    });
    p.targets.len() - 1
}

fn load_recipe_target(p: &mut Project, r: &Recipe, src_dir: &str) -> Result<(), String> {
    if p.targets.iter().any(|t| t.name == r.pkg.name) {
        return Ok(());
    }
    let i = add_target(p, &r.pkg.name, src_dir);
    p.targets[i].kind = TargetType::StaticLib;
    if let Some(bt) = &r.build.kind {
        p.targets[i].kind = match TargetType::parse(bt) {
            Some(k) => k,
            None => {
                return Err(format!("{}: unknown build type '{}'", r.pkg.name, bt));
            }
        };
    }
    p.targets[i].src = r.build.files.clone();
    p.targets[i].inc = r
        .build
        .include_dirs
        .iter()
        .map(|d| prefixed(src_dir, d))
        .collect();
    for d in &r.build.defines {
        p.targets[i].cflags.push(format!("-D{}", d));
    }
    p.targets[i].cflags.extend(r.build.cflags.iter().cloned());
    Ok(())
}

fn load_package(p: &mut Project, pkgdir: &str, depth: usize) -> Result<(), String> {
    if depth >= PKG_MAX_DEPTH {
        return Err(format!("package nesting deeper than {}", PKG_MAX_DEPTH));
    }
    let cfgp = cfg_path(pkgdir, "heddle.toml");
    if !path_exists(&cfgp) {
        return Err(format!("cannot read {}/heddle.toml", pkgdir));
    }
    let local = TomlCfg::parse(&cfgp)?;

    let names = local.sections("target.");
    for name in &names {
        let section = format!("target.{}", name);
        if p.targets.iter().any(|t| t.name == *name) {
            return Err(format!("{}: duplicate target '{}'", cfgp, name));
        }
        let i = add_target(p, name, &norm_dir(pkgdir));
        target_fill(&mut p.targets[i], &local, &section)?;
    }

    let mut pi = 0usize;
    while let Some(sub) = local.arr("package", "deps", pi) {
        pi += 1;
        let subdir = path(&norm_dir(pkgdir), sub);
        if !sys::is_dir(&subdir) {
            return Err(format!("{}: missing package directory '{}'", cfgp, sub));
        }
        load_package(p, &subdir, depth + 1)?;
    }
    Ok(())
}

fn load_store_recipes(p: &mut Project) -> Result<(), String> {
    let deps = p.pkg.deps.clone();
    for s in deps {
        if s.store_path.is_empty() || !recipe::match_dir(&s.store_path) {
            continue;
        }
        let r = recipe::load(&s.store_path)?;
        load_recipe_target(p, &r, &s.store_path)?;
    }
    Ok(())
}

fn auto_depends(p: &mut Project) -> Result<(), String> {
    for i in 0..p.pkg.deps.len() {
        let name = p.pkg.deps[i].name.clone();
        if !p.targets.iter().any(|t| t.name == name) {
            continue;
        }
        for k in 0..p.targets.len() {
            let ty = p.targets[k].kind;
            if ty != TargetType::Exe && ty != TargetType::SharedLib {
                continue;
            }
            if p.targets[k].name == name {
                continue;
            }
            if p.targets[k].deps.contains(&name) {
                continue;
            }
            p.targets[k].deps.push(name.clone());
        }
    }
    Ok(())
}

fn add_target_from_cfg(p: &mut Project, st: &StarTargetDef) -> Result<(), String> {
    if p.targets.iter().any(|t| t.name == st.name) {
        return Err(format!("duplicate target '{}'", st.name));
    }
    let i = add_target(p, &st.name, "");
    if let Some(ty) = &st.kind {
        p.targets[i].kind = match TargetType::parse(ty) {
            Some(k) => k,
            None => return Err(format!("target '{}': unknown type '{}'", st.name, ty)),
        };
    }
    p.targets[i].src = st.src.clone();
    p.targets[i].inc = st.inc.clone();
    p.targets[i].deps = st.deps.clone();
    p.targets[i].cflags = st.cflags.clone();
    p.targets[i].ldflags = st.ldflags.clone();
    p.targets[i].ldscript = st.ldscript.clone();
    p.targets[i].entry = st.entry.clone();
    p.targets[i].out = st.out.clone();
    p.targets[i].start_group = st.start_group;
    p.targets[i].whole_archive = st.whole_archive;

    if p.targets[i].kind != TargetType::Custom && p.targets[i].src.is_empty() {
        return Err(format!("target '{}': no sources", st.name));
    }
    Ok(())
}

fn find_platform<'a>(cfg: &'a StarCfg, name: &str) -> Option<&'a StarPlatform> {
    cfg.platforms.iter().find(|p| p.name == name)
}

fn project_load_star(
    p: &mut Project,
    root: &str,
    toolchain: Option<&str>,
) -> Result<(), String> {
    let cfgpath = format!("{}/heddle.star", root);
    let cfg = star::run(&cfgpath)?;

    let want = get_want_target();
    let mut plat: Option<&StarPlatform> = None;
    if let Some(w) = &want {
        for t in &cfg.targets {
            if &t.name != w {
                continue;
            }
            if let Some(pn) = &t.platform {
                if let Some(q) = find_platform(&cfg, pn) {
                    plat = Some(q);
                }
            }
        }
    }
    if plat.is_none() {
        for t in &cfg.targets {
            if let Some(pn) = &t.platform {
                if let Some(q) = find_platform(&cfg, pn) {
                    plat = Some(q);
                }
            }
        }
    }
    if plat.is_none() && !cfg.platforms.is_empty() {
        plat = Some(&cfg.platforms[0]);
    }

    if !cfg.platforms.is_empty() {
        for t in &cfg.targets {
            let pn = match &t.platform {
                Some(x) => x,
                None => continue,
            };
            for q in &cfg.platforms {
                if &q.name != pn {
                    continue;
                }
                if let Some(pp) = plat {
                    if !q.toolchain.is_empty()
                        && !pp.toolchain.is_empty()
                        && q.toolchain != pp.toolchain
                    {
                        return Err(format!(
                            "targets span toolchains '{}' and '{}'; build one platform at a time",
                            pp.toolchain, q.toolchain
                        ));
                    }
                }
            }
        }
    }

    p.build_dir = path(root, cfg.build_dir.as_deref().unwrap_or("out"));
    let tcname = toolchain
        .map(|s| s.to_string())
        .or_else(|| plat.and_then(|x| {
            if x.toolchain.is_empty() {
                None
            } else {
                Some(x.toolchain.clone())
            }
        }))
        .or_else(|| cfg.toolchain.clone())
        .unwrap_or_else(|| "auto".to_string());
    p.toolchain_name = tcname.clone();

    for i in 0..cfg.tc_name.len() {
        if cfg.tc_name[i] != tcname {
            continue;
        }
        p.tc_based = Some(cfg.tc_based[i].clone());
        p.tc_cc = Some(cfg.tc_cc[i].clone());
    }

    p.pkg.root = root.to_string();
    p.pkg.manifest = cfgpath.clone();
    p.pkg.lock_path = path(root, "heddle.lock");
    p.pkg.store = match &cfg.store {
        Some(s) => path(root, s),
        None => path(root, ".heddle/store"),
    };
    p.pkg.target.arch = plat.map(|x| x.arch.clone()).unwrap_or(cfg.arch.clone());
    p.pkg.target.abi = plat.map(|x| x.abi.clone()).unwrap_or(cfg.abi.clone());
    p.pkg.target.float_k = plat.map(|x| x.flt.clone()).unwrap_or(cfg.flt.clone());
    p.pkg.target.sysroot = plat.map(|x| x.sysroot.clone()).unwrap_or(cfg.sysroot.clone());

    for i in 0..cfg.tc_name.len() {
        pkg::manifest_add(
            &mut p.pkg,
            PkgKind::Toolchain,
            &cfg.tc_name[i],
            "",
            &cfg.tc_cc[i],
        );
    }
    for i in 0..cfg.dep_name.len() {
        pkg::manifest_add(
            &mut p.pkg,
            PkgKind::Library,
            &cfg.dep_name[i],
            &cfg.dep_ver[i],
            &cfg.dep_src[i],
        );
    }
    for st in &cfg.targets {
        add_target_from_cfg(p, st)?;
    }
    Ok(())
}

pub fn load(root: &str, toolchain: Option<&str>) -> Result<Project, String> {
    let mut p = Project {
        root: if root.is_empty() {
            ".".to_string()
        } else {
            root.to_string()
        },
        ..Default::default()
    };
    let star_path = format!("{}/heddle.star", p.root);
    let cfg_path_s = format!("{}/heddle.toml", p.root);
    if path_exists(&star_path) {
        let r = p.root.clone();
        return load_star_finish(p, &r, toolchain);
    }
    if !path_exists(&cfg_path_s) {
        return Err(format!("cannot read {} or {}", cfg_path_s, star_path));
    }

    let top = TomlCfg::parse(&cfg_path_s)?;
    let dir = top.str_of("build", "dir").map(|v| v.to_string());
    let tc = toolchain
        .map(|s| s.to_string())
        .or_else(|| top.str_of("build", "toolchain").map(|v| v.to_string()));
    p.build_dir = path(&p.root, dir.as_deref().unwrap_or("build"));
    p.toolchain_name = tc.unwrap_or_else(|| "auto".to_string());
    lang::init_builtin();
    lang::load_toml(&top);

    p.pkg = pkg::manifest_load(&p.root)?;
    p.target_prefix = p.pkg.toolchain_prefix.clone();
    p.target_sysroot = p.pkg.sysroot.clone();
    pkg::prepend_path(&mut p.pkg);
    let flags = format!("{} {}", p.pkg.target.cpu, p.pkg.target.fpu);
    toolchain::load_ex(
        &mut p.tc,
        &p.root,
        &p.toolchain_name,
        p.target_prefix.as_deref(),
        p.target_sysroot.as_deref(),
        &flags,
        &p.pkg.target.arch,
    )?;

    if !p.pkg.tools.is_empty() && !toolchain::tool_ok(&p.tc, &p.tc.cc.clone()) {
        return Err(format!(
            "managed toolchain '{}' is not installed (compiler '{}' not found); run 'heddle tool install'",
            p.pkg.tools[0].name, p.tc.cc
        ));
    }

    if !sys::is_dir(&p.build_dir) {
        sys::mkpath(&p.build_dir);
    }

    let nd = norm_dir(&p.root.clone());
    load_package(&mut p, &nd, 0)?;
    load_store_recipes(&mut p)?;
    auto_depends(&mut p)?;
    Ok(p)
}

fn load_star_finish(
    mut p: Project,
    root: &str,
    toolchain: Option<&str>,
) -> Result<Project, String> {
    lang::init_builtin();
    project_load_star(&mut p, root, toolchain)?;
    pkg::manifest_finalize(&mut p.pkg)?;
    p.target_prefix = p.pkg.toolchain_prefix.clone();
    p.target_sysroot = p.pkg.sysroot.clone();
    pkg::prepend_path(&mut p.pkg);
    let flags = format!("{} {}", p.pkg.target.cpu, p.pkg.target.fpu);

    if p.tc_cc.is_some() || p.tc_based.is_some() {
        toolchain::load_star(
            &mut p.tc,
            &toolchain::StarTcSpec {
                name: &p.toolchain_name,
                based: p.tc_based.as_deref(),
                cc: p.tc_cc.as_deref(),
                family: p.tc_family.as_deref(),
                prefix: p.target_prefix.as_deref(),
                sysroot: p.target_sysroot.as_deref(),
                extra_cflags: &flags,
                arch: &p.pkg.target.arch,
            },
        )?;
    } else {
        toolchain::load_ex(
            &mut p.tc,
            root,
            &p.toolchain_name,
            p.target_prefix.as_deref(),
            p.target_sysroot.as_deref(),
            &flags,
            &p.pkg.target.arch,
        )?;
    }

    if !sys::is_dir(&p.build_dir) {
        sys::mkpath(&p.build_dir);
    }
    load_store_recipes(&mut p)?;
    auto_depends(&mut p)?;
    Ok(p)
}

pub fn target<'a>(p: &'a Project, name: &str) -> Option<&'a Target> {
    p.targets.iter().find(|t| t.name == name)
}
