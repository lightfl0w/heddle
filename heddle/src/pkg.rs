use std::fs;

use crate::model::{PkgKind, PkgManifest, PkgSource, PkgSpec, TargetProfile, Toolchain};
use crate::sys;
use crate::toml::Toml;
use loom_core::hash::{hash_read_file, hash_text, hash_u64, HASH_FNV_OFFSET};
use loom_core::proc;

const ARCH_TABLE: &[(&str, &str, &str, &str, &str)] = &[
    ("armv6m", "arm-none-eabi-", "-mthumb -mcpu=cortex-m0", "", "-mfpu=none"),
    ("armv7m", "arm-none-eabi-", "-mthumb -mcpu=cortex-m3", "", "-mfpu=none"),
    ("armv7em", "arm-none-eabi-", "-mthumb -mcpu=cortex-m4", "", "-mfpu=fpv4-sp-d16"),
    ("armv8m", "arm-none-eabi-", "-mthumb -mcpu=cortex-m33", "", "-mfpu=fpv5-sp-d16"),
    ("cortex-m4", "arm-none-eabi-", "-mthumb -mcpu=cortex-m4", "", "-mfpu=fpv4-sp-d16"),
    ("cortex-m7", "arm-none-eabi-", "-mthumb -mcpu=cortex-m7", "", "-mfpu=fpv5-d16"),
    ("armv7a", "arm-none-eabi-", "-mcpu=cortex-a7", "", "-mfpu=neon-vfpv4"),
    ("aarch64", "aarch64-none-elf-", "-march=armv8-a", "", ""),
    ("riscv32imac", "riscv64-unknown-elf-", "-march=rv32imac -mabi=ilp32", "", ""),
    ("riscv32", "riscv64-unknown-elf-", "-march=rv32i -mabi=ilp32", "", ""),
    ("riscv64", "riscv64-unknown-elf-", "-march=rv64imac -mabi=lp64", "", ""),
    ("xtensa-lx106", "xtensa-esp32-elf-", "-mlongcalls", "", ""),
    ("xtensa", "xtensa-esp32-elf-", "-mlongcalls", "", ""),
    ("msp430", "msp430-elf-", "-mmcu=msp430f5529", "", ""),
    ("avr", "avr-", "-mmcu=atmega328p", "", ""),
    ("powerpc", "powerpc-eabi-", "-mcpu=e500mc", "", ""),
    ("x86_64", "", "-m64", "", ""),
    ("i686", "", "-m32", "", ""),
];

const VCPKG_TRIPLETS: &[(&str, &str, &str)] = &[
    ("x86_64", "", "x64-linux"),
    ("i686", "", "x86-linux"),
    ("aarch64", "", "arm64-linux"),
    ("armv7a", "hard", "arm-linux"),
    ("armv7m", "", "thumbv7m-none-eabi"),
    ("armv7em", "hard", "thumbv7em-none-eabihf"),
    ("armv7em", "", "thumbv7em-none-eabihf"),
];

fn path_join(a: &str, b: &str) -> String {
    if a.is_empty() || a == "." {
        return b.to_string();
    }
    format!("{}/{}", a, b)
}

fn path_join3(a: &str, b: &str, c: &str) -> String {
    path_join(&path_join(a, b), c)
}

fn exists(p: &str) -> bool {
    sys::stat(p).is_some()
}

fn dir_exists(p: &str) -> bool {
    sys::is_dir(p)
}

fn kind_dir(k: PkgKind) -> &'static str {
    if k == PkgKind::Toolchain {
        "toolchain"
    } else {
        "library"
    }
}

fn source_name(s: PkgSource) -> &'static str {
    match s {
        PkgSource::Source => "source",
        PkgSource::Binary => "binary",
        _ => "auto",
    }
}

fn parse_source(s: &str) -> PkgSource {
    match s {
        "source" | "src" => PkgSource::Source,
        "binary" | "bin" => PkgSource::Binary,
        _ => PkgSource::Auto,
    }
}

fn split_at(s: &str) -> (String, String) {
    match s.rfind('@') {
        Some(i) if i != 0 => (s[..i].to_string(), s[i + 1..].to_string()),
        _ => (s.to_string(), "latest".to_string()),
    }
}

fn pad(s: &str, w: usize) -> String {
    let mut out = s.to_string();
    while out.len() < w {
        out.push(' ');
    }
    out
}

fn target_resolve(t: &mut TargetProfile) -> Result<(), String> {
    if t.arch.is_empty() {
        return Ok(());
    }

    let a = match ARCH_TABLE.iter().find(|x| x.0 == t.arch) {
        Some(a) => *a,
        None => return Err(format!("[target] unknown arch '{}'", t.arch)),
    };

    if t.prefix.is_empty() {
        t.prefix = a.1.to_string();
    }

    if t.cpu.is_empty() {
        t.cpu = a.2.to_string();
    }

    if t.fpu.is_empty() {
        let has_fpu = !a.3.is_empty() || !a.4.is_empty();
        t.fpu = if !has_fpu {
            String::new()
        } else if t.float_k == "soft" {
            "-mfloat-abi=soft".to_string()
        } else if t.float_k == "hard" {
            format!("{} -mfloat-abi=hard", a.4)
        } else if t.float_k == "softfp" {
            format!("{} -mfloat-abi=softfp", a.3)
        } else {
            a.3.to_string()
        };
    }

    Ok(())
}

fn split_flags(text: &str) -> Vec<String> {
    let b = text.as_bytes();
    let mut out: Vec<String> = Vec::new();
    let mut p = 0usize;

    while p < b.len() {
        while p < b.len() && matches!(b[p], b' ' | b'\t' | b'\n' | b'\r') {
            p += 1;
        }
        if p >= b.len() {
            break;
        }

        let mut buf: Vec<u8> = Vec::new();
        let mut q: u8 = 0;

        while p < b.len() {
            let c = b[p];
            if q == 0 && matches!(c, b' ' | b'\t' | b'\n' | b'\r') {
                break;
            }
            if c == b'"' || c == b'\'' {
                if q == c {
                    q = 0;
                } else if q == 0 {
                    q = c;
                }
                p += 1;
                continue;
            }
            if buf.len() + 1 < 4096 {
                buf.push(c);
            }
            p += 1;
        }

        if !buf.is_empty() {
            out.push(String::from_utf8_lossy(&buf).into_owned());
        }
    }

    out
}

fn pc_log_path() -> String {
    if cfg!(windows) {
        for k in ["TEMP", "TMP"] {
            if let Ok(v) = std::env::var(k) {
                if !v.is_empty() {
                    return v;
                }
            }
        }
        return ".".to_string();
    }
    match std::env::var("TMPDIR") {
        Ok(v) if !v.is_empty() => v,
        _ => "/tmp".to_string(),
    }
}

fn pc_query(pc: &str, mode: &str, ran: &mut bool) -> Option<String> {
    *ran = false;

    let log = format!(
        "{}/heddle-pkgconfig-{}.log",
        pc_log_path(),
        std::process::id()
    );
    let argv = vec!["pkg-config".to_string(), mode.to_string(), pc.to_string()];

    let r = match proc::run(&argv, None, &log, &[]) {
        Some(r) => r,
        None => return None,
    };

    *ran = true;

    if r.exit_code != 0 {
        return None;
    }

    let data = match fs::read(&log) {
        Ok(d) => d,
        Err(_) => return None,
    };

    let _ = fs::remove_file(&log);

    let n = if data.len() > 65535 { 65535 } else { data.len() };

    Some(String::from_utf8_lossy(&data[..n]).into_owned())
}

pub fn pkgconfig_resolve(s: &mut PkgSpec) -> Result<(), String> {
    let pc = if s.pc_name.is_empty() {
        s.name.clone()
    } else {
        s.pc_name.clone()
    };

    let mut ran_c = false;
    let mut ran_l = false;
    let cf = pc_query(&pc, "--cflags", &mut ran_c);
    let lf = pc_query(&pc, "--libs", &mut ran_l);

    if !ran_c && !ran_l {
        return Err("cannot run pkg-config (is it installed and on PATH?)".to_string());
    }

    if cf.is_none() && lf.is_none() {
        return Err(format!(
            "pkg-config does not know '{}'; install its development package or set PKG_CONFIG_PATH",
            pc
        ));
    }

    s.cflags = split_flags(cf.as_deref().unwrap_or(""));
    s.ldflags = split_flags(lf.as_deref().unwrap_or(""));

    Ok(())
}

fn spec_seen(l: &[PkgSpec], name: &str, version: &str) -> bool {
    l.iter().any(|s| s.name == name && s.version == version)
}

fn spec_finish(
    t: &Toml,
    section: &str,
    kind: PkgKind,
    name: &str,
    version: &str,
    val: &str,
    out: &mut Vec<PkgSpec>,
) {
    let mut s = PkgSpec {
        kind,
        name: name.to_string(),
        version: if version.is_empty() {
            "latest".to_string()
        } else {
            version.to_string()
        },
        ..Default::default()
    };

    let sub = format!("{}.{}", section, name);
    let pcfield = t.str_of(&sub, "pkgconfig");

    if let Some(pc) = pcfield {
        s.source = PkgSource::PkgConfig;
        s.pc_name = if pc.is_empty() {
            s.name.clone()
        } else {
            pc.to_string()
        };
    } else if val.starts_with("pkgconfig") {
        let b = val.as_bytes();
        let sep = b.len() == 9 || (b.len() > 9 && b[9] == b':');
        if sep {
            s.source = PkgSource::PkgConfig;
            s.pc_name = if b.len() > 10 && b[9] == b':' {
                val[10..].to_string()
            } else {
                s.name.clone()
            };
        } else {
            s.source = parse_source(t.str_of(&sub, "source").unwrap_or(""));
        }
    } else {
        s.source = parse_source(t.str_of(&sub, "source").unwrap_or(""));
    }

    if let Some(tgt) = t.str_of(&sub, "target") {
        s.target = tgt.to_string();
    }

    out.push(s);
}

fn load_section(t: &Toml, section: &str, kind: PkgKind, out: &mut Vec<PkgSpec>) {
    if let Some(tab) = t.tables.iter().rev().find(|x| x.name == section) {
        for (key, val) in &tab.items {
            if key.contains('[') {
                continue;
            }

            let has_at = val.find('@');
            let has_key_at = key.contains('@');

            let (name, version) = match has_at {
                Some(0) => {
                    if has_key_at {
                        let (n, mut v) = split_at(key);
                        if !val.is_empty() && val != "auto" {
                            v = val.clone();
                        }
                        (n, v)
                    } else {
                        (
                            key.clone(),
                            if val.is_empty() {
                                "latest".to_string()
                            } else {
                                val.clone()
                            },
                        )
                    }
                }
                Some(_) => split_at(val),
                None => {
                    if has_key_at {
                        let (n, mut v) = split_at(key);
                        if !val.is_empty() && val != "auto" {
                            v = val.clone();
                        }
                        (n, v)
                    } else {
                        (
                            key.clone(),
                            if val.is_empty() {
                                "latest".to_string()
                            } else {
                                val.clone()
                            },
                        )
                    }
                }
            };

            spec_finish(t, section, kind, &name, &version, val, out);
        }
    }

    let plen = section.len();
    for tb in &t.tables {
        if !tb.name.starts_with(section) {
            continue;
        }
        let rest = &tb.name[plen..];
        if !rest.starts_with('.') {
            continue;
        }
        let name = &rest[1..];
        if name.contains('.') || spec_seen(out, name, "latest") {
            continue;
        }
        spec_finish(t, section, kind, name, "latest", "", out);
    }
}

fn has_recipe(dir: &str) -> bool {
    exists(&path_join(dir, "package.toml"))
}

fn resolve_store(m: &mut PkgManifest) {
    let store = m.store.clone();
    for pass in 0..2 {
        let list = if pass == 0 { &mut m.tools } else { &mut m.deps };
        for s in list.iter_mut() {
            let p = path_join3(&store, kind_dir(s.kind), &s.name);
            s.store_path = path_join(&p, &s.version);
            s.recipe = has_recipe(&s.store_path);
        }
    }
}

fn resolve_pkgconfig(m: &mut PkgManifest) -> Result<(), String> {
    for s in m.deps.iter_mut() {
        if s.source != PkgSource::PkgConfig {
            continue;
        }
        pkgconfig_resolve(s)?;
    }
    Ok(())
}

pub fn manifest_finalize(m: &mut PkgManifest) -> Result<(), String> {
    target_resolve(&mut m.target)?;
    m.toolchain_prefix = Some(m.target.prefix.clone());
    m.sysroot = Some(m.target.sysroot.clone());
    resolve_store(m);
    resolve_pkgconfig(m)
}

pub fn manifest_add(m: &mut PkgManifest, kind: PkgKind, name: &str, version: &str, source: &str) {
    let s = PkgSpec {
        kind,
        name: name.to_string(),
        version: if version.is_empty() {
            "latest".to_string()
        } else {
            version.to_string()
        },
        source: if source.is_empty() {
            PkgSource::Auto
        } else {
            parse_source(source)
        },
        ..Default::default()
    };

    if kind == PkgKind::Toolchain {
        m.tools.push(s);
    } else {
        m.deps.push(s);
    }
}

fn manifest_load_star(mut m: PkgManifest, star_path: &str) -> Result<PkgManifest, String> {
    let cfg = crate::star::run(star_path)?;

    m.store = match &cfg.store {
        Some(s) => path_join(&m.root, s),
        None => path_join3(&m.root, ".heddle", "store"),
    };

    let plat = cfg.platforms.first();

    m.target.arch = match plat {
        Some(p) if !p.arch.is_empty() => p.arch.clone(),
        _ => cfg.arch.clone(),
    };
    m.target.abi = match plat {
        Some(p) if !p.abi.is_empty() => p.abi.clone(),
        _ => cfg.abi.clone(),
    };
    m.target.float_k = match plat {
        Some(p) if !p.flt.is_empty() => p.flt.clone(),
        _ => cfg.flt.clone(),
    };
    m.target.sysroot = match plat {
        Some(p) if !p.sysroot.is_empty() => p.sysroot.clone(),
        _ => cfg.sysroot.clone(),
    };

    for i in 0..cfg.tc_name.len() {
        let cc = cfg.tc_cc.get(i).cloned().unwrap_or_default();
        manifest_add(&mut m, PkgKind::Toolchain, &cfg.tc_name[i], "", &cc);
    }

    for i in 0..cfg.dep_name.len() {
        let ver = cfg.dep_ver.get(i).cloned().unwrap_or_default();
        let src = cfg.dep_src.get(i).cloned().unwrap_or_default();
        manifest_add(&mut m, PkgKind::Library, &cfg.dep_name[i], &ver, &src);
    }

    manifest_finalize(&mut m)?;
    Ok(m)
}

pub fn manifest_load(root: &str) -> Result<PkgManifest, String> {
    let mut m = PkgManifest::default();
    m.root = root.to_string();
    let cfg = format!("{}/heddle.toml", m.root);
    let star = format!("{}/heddle.star", m.root);
    m.lock_path = path_join(&m.root, "heddle.lock");

    if exists(&star) {
        m.manifest = star.clone();
        return manifest_load_star(m, &star);
    }

    m.manifest = cfg.clone();
    if !exists(&cfg) {
        return Err(format!("cannot read {} or {}", cfg, star));
    }

    let t = Toml::parse(&cfg)?;

    m.store = match t.str_of("build", "store") {
        Some(s) => path_join(&m.root, s),
        None => path_join3(&m.root, ".heddle", "store"),
    };
    m.target.arch = t.str_of("target", "arch").unwrap_or("").to_string();
    m.target.abi = t.str_of("target", "abi").unwrap_or("").to_string();
    m.target.float_k = t.str_of("target", "float").unwrap_or("").to_string();
    m.target.sysroot = t.str_of("target", "sysroot").unwrap_or("").to_string();

    load_section(&t, "toolchain", PkgKind::Toolchain, &mut m.tools);
    load_section(&t, "dependencies", PkgKind::Library, &mut m.deps);

    manifest_finalize(&mut m)?;

    Ok(m)
}

fn list_dir(path: &str) -> Option<Vec<String>> {
    let rd = fs::read_dir(path).ok()?;
    let mut out: Vec<String> = Vec::new();
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if name == "." || name == ".." {
            continue;
        }
        let full = path_join(path, &name);
        if sys::is_dir(&full) {
            out.push(format!("{}/", full));
        } else {
            out.push(full);
        }
    }
    Some(out)
}

fn rel_of(root: &str, full: &str) -> String {
    let mut r = if full.len() >= root.len() {
        &full[root.len()..]
    } else {
        full
    };
    let b = r.as_bytes();
    let mut i = 0usize;
    while i < b.len() && (b[i] == b'/' || b[i] == b'\\') {
        i += 1;
    }
    r = &r[i..];
    r.to_string()
}

fn is_dir_name(r: &str) -> bool {
    let b = r.as_bytes();
    !b.is_empty() && (b[b.len() - 1] == b'/' || b[b.len() - 1] == b'\\')
}

fn hash_tree_rec(dir: &str, base: &str, h0: u64) -> u64 {
    let mut h = h0;
    let mut items = match list_dir(dir) {
        Some(v) => v,
        None => return h,
    };

    items.sort();

    for it in &items {
        let name = rel_of(dir, it);
        let rel = format!("{}{}", base, name);
        h = hash_text(h, &rel);
        if is_dir_name(&name) {
            let sub = format!("{}/", rel);
            h = hash_tree_rec(it, &sub, h);
        } else {
            h = hash_u64(h, hash_read_file(None, it));
        }
    }

    h
}

pub fn hash_tree(dir: &str) -> String {
    let h = hash_tree_rec(dir, "", HASH_FNV_OFFSET);
    format!("fnv64:{:016x}", h)
}

fn copy_file(src: &str, dst: &str) -> bool {
    fs::copy(src, dst).is_ok()
}

pub fn copy_tree(src: &str, dst: &str) -> bool {
    sys::mkpath(dst);

    let items = match list_dir(src) {
        Some(v) => v,
        None => return false,
    };

    for it in &items {
        let rel = rel_of(src, it);
        let d = path_join(dst, &rel);
        let ok = if is_dir_name(&rel) {
            copy_tree(it, &d)
        } else {
            copy_file(it, &d)
        };
        if !ok {
            return false;
        }
    }

    true
}

fn remove_tree(path: &str) {
    if let Some(items) = list_dir(path) {
        for it in &items {
            if is_dir_name(it) {
                remove_tree(it);
            } else {
                let _ = fs::remove_file(it);
            }
        }
    }

    if sys::is_dir(path) {
        let _ = fs::remove_dir(path);
    } else {
        let _ = fs::remove_file(path);
    }
}

fn sh_log_path() -> String {
    format!("{}/heddle-sh-{}.log", pc_log_path(), std::process::id())
}

fn sh(cmd: &str) -> bool {
    let log = sh_log_path();
    let ok = match proc::shell(cmd, None, &log) {
        Some(r) => r.exit_code == 0,
        None => false,
    };
    let _ = fs::remove_file(&log);
    ok
}

fn is_payload_dir(r: &str) -> bool {
    r == "bin/" || r == "include/" || r == "lib/" || r == "src/"
}

fn hoist(dst: &str, wrap: &str) {
    let items = match list_dir(wrap) {
        Some(v) => v,
        None => return,
    };

    for it in &items {
        let d = path_join(dst, &rel_of(wrap, it));
        if fs::rename(it, &d).is_err() {
            copy_tree(it, &d);
        }
    }

    remove_tree(wrap);
}

fn unpack(archive: &str, dst: &str) -> bool {
    sys::mkpath(dst);

    if !sh(&format!(
        "tar -xzf '{}' -C '{}' >/dev/null 2>&1",
        archive, dst
    )) {
        return false;
    }

    for _ in 0..8 {
        let items = match list_dir(dst) {
            Some(v) => v,
            None => break,
        };

        if items.len() != 1 || !is_dir_name(&items[0]) {
            break;
        }

        if is_payload_dir(&rel_of(dst, &items[0])) {
            break;
        }

        hoist(dst, &items[0]);
    }

    true
}

fn vcpkg_triplet_for(tp: &TargetProfile) -> Option<&'static str> {
    if tp.arch.is_empty() {
        return None;
    }

    for (arch, flt, tri) in VCPKG_TRIPLETS {
        if *arch != tp.arch {
            continue;
        }
        if !flt.is_empty() && *flt != tp.float_k {
            continue;
        }
        return Some(tri);
    }

    None
}

fn vcpkg_is_registry(dir: &str) -> bool {
    dir_exists(&path_join(dir, "ports")) && dir_exists(&path_join(dir, "versions"))
}

fn write_libs(libdir: &str, meta: &str) {
    let items = match list_dir(libdir) {
        Some(v) => v,
        None => return,
    };

    let mut out = String::new();

    for it in &items {
        if is_dir_name(it) {
            continue;
        }
        let base = match it.rfind('/') {
            Some(i) => &it[i + 1..],
            None => it.as_str(),
        };
        if !base.starts_with("lib") {
            continue;
        }
        let dot = match base.rfind('.') {
            Some(d) => d,
            None => continue,
        };
        if dot <= 3 {
            continue;
        }
        out.push_str(&base[3..dot]);
        out.push('\n');
    }

    let _ = fs::write(meta, out.as_bytes());
}

fn vcpkg_import(
    installed: &str,
    triplet: &str,
    store: &str,
    name: &str,
    version: &str,
) -> Result<String, String> {
    let tree = path_join(installed, triplet);

    if !dir_exists(&tree) {
        return Err(format!("no installed tree at {}", tree));
    }

    let ver = if version.is_empty() { "0" } else { version };
    let dst = path_join(&path_join3(store, "library", name), ver);

    sys::mkpath(&dst);

    let mut copied = 0;

    for payload in ["include", "lib", "bin", "share"] {
        let src = path_join(&tree, payload);
        if dir_exists(&src) {
            let sub = path_join(&dst, payload);
            if copy_tree(&src, &sub) {
                copied += 1;
            }
        }
    }

    if copied == 0 {
        return Err(format!(
            "installed tree {} has no include/lib/bin/share",
            triplet
        ));
    }

    write_libs(&path_join(&dst, "lib"), &path_join(&dst, ".heddle-pkg"));

    Ok(hash_tree(&dst))
}

fn registry() -> Option<String> {
    match std::env::var("HEDDLE_REGISTRY") {
        Ok(v) if !v.is_empty() => Some(v),
        _ => None,
    }
}

fn is_http(u: &str) -> bool {
    u.starts_with("http://") || u.starts_with("https://")
}

fn fetch_pkg(
    s: &PkgSpec,
    dst: &str,
    store: &str,
    vcpkg_root_dir: Option<&str>,
    triplet: Option<&str>,
    offline: bool,
) -> Result<(), String> {
    if offline {
        return Err(format!(
            "{} {}@{} not in store and offline",
            kind_dir(s.kind),
            s.name,
            s.version
        ));
    }

    let mut keep = String::new();

    if let (Some(vroot), Some(trip)) = (vcpkg_root_dir, triplet) {
        if !vroot.is_empty() && s.kind == PkgKind::Library {
            let inst = path_join(vroot, "installed");
            match vcpkg_import(&inst, trip, store, &s.name, &s.version) {
                Ok(_) => return Ok(()),
                Err(e) => keep = e,
            }
        }
    }

    let reg = match registry() {
        Some(r) => r,
        None => {
            return Err(format!(
                "{} {}@{} not available: {}{}set VCPKG_ROOT to an installed vcpkg tree (or HEDDLE_REGISTRY)",
                kind_dir(s.kind),
                s.name,
                s.version,
                if keep.is_empty() { "not found; " } else { keep.as_str() },
                if keep.is_empty() { "" } else { "; " }
            ));
        }
    };

    let rel = format!("{}/{}/{}", kind_dir(s.kind), s.name, s.version);

    if is_http(&reg) {
        let url = format!("{}/{}.tar.gz", reg, rel);
        let tmp = format!("{}.tmp.tgz", dst);
        if !sh(&format!(
            "curl -fsS -o '{}' '{}' >/dev/null 2>&1",
            tmp, url
        )) {
            return Err(format!("cannot fetch {}", url));
        }

        let rc = unpack(&tmp, dst);
        let _ = fs::remove_file(&tmp);

        return if rc {
            Ok(())
        } else {
            Err(format!("cannot unpack {}", url))
        };
    }

    let dir = path_join(&reg, &rel);
    let targz = format!("{}.tar.gz", dir);

    let mut ok = false;
    let mut msg = String::new();

    if dir_exists(&dir) {
        ok = copy_tree(&dir, dst);
    } else if exists(&targz) {
        ok = unpack(&targz, dst);
    } else if vcpkg_is_registry(&reg) {
        let port = path_join3(&reg, "ports", &s.name);
        if dir_exists(&port) {
            msg = format!(
                "vcpkg port '{}' needs a built tree; run 'heddle vcpkg import' first",
                s.name
            );
        } else {
            msg = format!("vcpkg registry has no port '{}'", s.name);
        }
    } else {
        msg = format!("registry has no {}", rel);
    }

    if ok {
        Ok(())
    } else {
        Err(msg)
    }
}

fn lock_lookup(path: &str, kind: &str, name: &str, version: &str) -> Option<String> {
    let text = sys::read_to_string(path)?;

    for line in text.lines() {
        if line.starts_with('#') {
            continue;
        }
        let mut t = line.split_whitespace();
        let k = t.next();
        let n = t.next();
        let v = t.next();
        let h = t.next();
        let (k, n, v, h) = match (k, n, v, h) {
            (Some(a), Some(b), Some(c), Some(d)) => (a, b, c, d),
            _ => continue,
        };
        if k != kind || n != name || v != version {
            continue;
        }
        return Some(h.to_string());
    }

    None
}

fn lock_write(m: &PkgManifest) -> Result<(), String> {
    let mut out = String::from("# generated by heddle; do not edit\n");

    for pass in 0..2 {
        let (list, k) = if pass == 0 {
            (&m.tools, "toolchain")
        } else {
            (&m.deps, "library")
        };
        for s in list {
            if !s.hash.is_empty() {
                out.push_str(&format!("{} {} {} {}\n", k, s.name, s.version, s.hash));
            }
        }
    }

    if fs::write(&m.lock_path, out.as_bytes()).is_err() {
        return Err(format!("cannot write {}", m.lock_path));
    }

    Ok(())
}

fn chmod_bin(dir: &str) {
    let bin = path_join(dir, "bin");
    if !dir_exists(&bin) {
        return;
    }
    if let Some(items) = list_dir(&bin) {
        for it in &items {
            if !is_dir_name(it) {
                sys::chmod(it, 0o755);
            }
        }
    }
}

fn which_prog(prog: &str) -> Option<String> {
    let path = std::env::var("PATH").ok()?;

    let exts: &[&str] = if cfg!(windows) {
        &[".exe", ".bat", ".cmd"]
    } else {
        &[""]
    };

    for dir in sys::path_dirs(&path) {
        for e in exts {
            let c = format!("{}/{}{}", dir, prog, e);
            if let Some(st) = sys::stat(&c) {
                if !st.is_dir {
                    return Some(c);
                }
            }
        }
    }

    None
}

fn vcpkg_root() -> Option<String> {
    for k in ["VCPKG_ROOT", "HEDDLE_VCPKG_ROOT"] {
        if let Ok(v) = std::env::var(k) {
            if !v.is_empty() {
                return Some(v);
            }
        }
    }

    let mut home = std::env::var("HOME").ok();
    if home.is_none() && cfg!(windows) {
        home = std::env::var("USERPROFILE").ok();
    }

    let fixed: &[&str] = if cfg!(windows) {
        &["C:/vcpkg", "C:/src/vcpkg", "C:/tools/vcpkg", "C:/Program Files/vcpkg"]
    } else {
        &["/opt/vcpkg", "/usr/local/vcpkg", "/usr/share/vcpkg"]
    };

    for f in fixed {
        if dir_exists(&path_join(f, "installed")) {
            return Some(f.to_string());
        }
    }

    if let Some(h) = &home {
        for rel in ["vcpkg", "src/vcpkg", ".vcpkg"] {
            let cand = path_join(h, rel);
            if dir_exists(&path_join(&cand, "installed")) {
                return Some(cand);
            }
        }
    }

    if let Some(exe) = which_prog("vcpkg") {
        if let Some(slash) = exe.rfind('/') {
            let bindir = &exe[..slash];
            if let Some(up) = bindir.rfind('/') {
                let root = &bindir[..up];
                if dir_exists(&path_join(root, "installed")) {
                    return Some(root.to_string());
                }
            }
        }
    }

    None
}

fn install_list(
    m: &mut PkgManifest,
    is_deps: bool,
    offline: bool,
    verbose: bool,
) -> Result<(), String> {
    let vroot = vcpkg_root();
    let vtrip = vcpkg_triplet_for(&m.target);

    if verbose && vroot.is_some() && is_deps {
        eprintln!(
            "heddle: vcpkg root {} ({})",
            vroot.as_deref().unwrap_or(""),
            vtrip.unwrap_or("no triplet for this target")
        );
    }

    let store = m.store.clone();
    let lock_path = m.lock_path.clone();

    let list = if is_deps { &mut m.deps } else { &mut m.tools };

    for s in list.iter_mut() {
        if s.source == PkgSource::PkgConfig {
            continue;
        }

        let dst = s.store_path.clone();

        let want = lock_lookup(&lock_path, kind_dir(s.kind), &s.name, &s.version);
        let mut drift = false;

        if dir_exists(&dst) {
            let hex = hash_tree(&dst);
            drift = match &want {
                Some(w) => *w != hex,
                None => false,
            };
        }

        if !dir_exists(&dst) || drift {
            if drift && offline {
                return Err(format!(
                    "{} {}@{} drifted and --offline forbids repair",
                    kind_dir(s.kind),
                    s.name,
                    s.version
                ));
            }

            if dir_exists(&dst) {
                remove_tree(&dst);
            }

            if verbose {
                eprintln!(
                    "heddle: {} {} {}@{}",
                    if drift { "repairing" } else { "fetching" },
                    kind_dir(s.kind),
                    s.name,
                    s.version
                );
            }

            fetch_pkg(
                s,
                &dst,
                &store,
                vroot.as_deref(),
                vtrip,
                offline,
            )?;
        }

        s.store_path = dst.clone();

        if s.kind == PkgKind::Toolchain {
            chmod_bin(&dst);
        }

        let hex = hash_tree(&dst);

        if let Some(w) = &want {
            if *w != hex {
                eprintln!(
                    "heddle: warning: {} {}@{} content changed (lock {}, store {})",
                    kind_dir(s.kind),
                    s.name,
                    s.version,
                    w,
                    hex
                );
            }
        }

        s.hash = hex;

        if verbose {
            eprintln!("heddle:   store {} ({})", dst, s.hash);
        }
    }

    Ok(())
}

pub fn install(m: &mut PkgManifest, offline: bool, verbose: bool) -> Result<(), String> {
    if m.tools.is_empty() && m.deps.is_empty() {
        if verbose {
            eprintln!("heddle: nothing to install");
        }
        return Ok(());
    }

    sys::mkpath(&m.store);

    if verbose {
        eprintln!("heddle: store {}", m.store);
    }

    install_list(m, false, offline, verbose)?;
    install_list(m, true, offline, verbose)?;

    lock_write(m)
}

pub fn verify(m: &mut PkgManifest, verbose: bool) -> Result<(), String> {
    if !exists(&m.lock_path) {
        return Err(format!(
            "no lockfile at {}; run 'heddle tool install'",
            m.lock_path
        ));
    }

    let mut bad = 0;

    for pass in 0..2 {
        let lock_path = m.lock_path.clone();
        let list = if pass == 0 { &m.tools } else { &m.deps };

        for s in list {
            if !dir_exists(&s.store_path) {
                eprintln!(
                    "heddle: {} {}@{}: not installed",
                    kind_dir(s.kind),
                    s.name,
                    s.version
                );
                bad += 1;
                continue;
            }

            let hex = hash_tree(&s.store_path);
            let want = lock_lookup(&lock_path, kind_dir(s.kind), &s.name, &s.version);

            match want {
                None => {
                    eprintln!(
                        "heddle: {} {}@{}: missing from lockfile",
                        kind_dir(s.kind),
                        s.name,
                        s.version
                    );
                    bad += 1;
                }
                Some(w) => {
                    if w != hex {
                        eprintln!(
                            "heddle: {} {}@{}: drift (lock {}, store {})",
                            kind_dir(s.kind),
                            s.name,
                            s.version,
                            w,
                            hex
                        );
                        bad += 1;
                    } else if verbose {
                        eprintln!(
                            "heddle: ok {} {}@{}",
                            kind_dir(s.kind),
                            s.name,
                            s.version
                        );
                    }
                }
            }
        }
    }

    if bad > 0 {
        return Err(format!("environment does not match {}", m.lock_path));
    }

    Ok(())
}

pub fn plan(m: &PkgManifest) {
    let mut out = String::from("heddle: plan\n");

    if !m.target.arch.is_empty() {
        out.push_str(&format!("  architecture: {}", m.target.arch));
        if !m.target.abi.is_empty() {
            out.push_str(&format!(" abi={}", m.target.abi));
        }
        if !m.target.float_k.is_empty() {
            out.push_str(&format!(" float={}", m.target.float_k));
        }
        out.push_str(&format!(
            "\n  prefix:       {}\n  cflags:      {} {}\n",
            m.target.prefix, m.target.cpu, m.target.fpu
        ));
    }

    for pass in 0..2 {
        let list = if pass == 0 { &m.tools } else { &m.deps };
        for s in list {
            out.push_str(&format!(
                "  {} {} {} {}\n",
                pad(kind_dir(s.kind), 10),
                pad(&s.name, 16),
                pad(&s.version, 10),
                source_name(s.source)
            ));
        }
    }

    print!("{}", out);
}

fn tool_path(m: &PkgManifest) -> String {
    let mut s = String::new();

    for t in &m.tools {
        let bin = path_join(&t.store_path, "bin");
        if dir_exists(&bin) {
            if !s.is_empty() {
                s.push(':');
            }
            s.push_str(&bin);
        }
    }

    if !s.is_empty() {
        s.push(':');
    }
    s.push_str(&std::env::var("PATH").unwrap_or_default());

    s
}

pub fn env(m: &PkgManifest) -> Vec<String> {
    let mut cflags = format!("{} {}", m.target.cpu, m.target.fpu);
    let mut ldflags = String::new();
    let mut cpath = String::new();

    for s in &m.deps {
        let inc = path_join(&s.store_path, "include");
        let lib = path_join(&s.store_path, "lib");

        if dir_exists(&inc) {
            cflags.push_str(&format!(" -I{}", inc));
            if !cpath.is_empty() {
                cpath.push(':');
            }
            cpath.push_str(&inc);
        }

        if dir_exists(&lib) {
            ldflags.push_str(&format!(" -L{}", lib));
            ldflags.push_str(&format!(" -l{}", s.name));
        }
    }

    let pv = tool_path(m);

    let mut v = vec![
        format!("PATH={}", pv),
        format!("CFLAGS={}", cflags),
        format!("CXXFLAGS={}", cflags),
        format!("LDFLAGS={}", ldflags),
    ];

    if !cpath.is_empty() {
        v.push(format!("CPATH={}", cpath));
    }

    v
}

pub fn prepend_path(m: &mut PkgManifest) {
    if m.tools.is_empty() {
        return;
    }

    let full = tool_path(m);
    std::env::set_var("PATH", full);
}

pub fn variant_key(t: &TargetProfile, tc: &Toolchain) -> String {
    let mut h = HASH_FNV_OFFSET;

    for p in [
        &t.arch,
        &t.abi,
        &t.float_k,
        &t.cpu,
        &t.fpu,
        &tc.cc,
        &tc.platform,
    ] {
        if !p.is_empty() {
            h = hash_text(h, p);
        }
    }

    for c in &tc.cflags {
        h = hash_text(h, c);
    }

    format!(
        "{}-{}-{:016x}",
        if t.arch.is_empty() { "host" } else { t.arch.as_str() },
        if tc.name.is_empty() { "cc" } else { tc.name.as_str() },
        h
    )
}

pub fn lock_add(
    lock: &str,
    kind: &str,
    name: &str,
    version: &str,
    hash: &str,
) -> Result<(), String> {
    let tmp = format!("{}.tmp", lock);
    let mut out = String::from("# generated by heddle; do not edit\n");
    let mut written = false;

    if let Some(text) = sys::read_to_string(lock) {
        for line in text.lines() {
            if line.starts_with('#') {
                continue;
            }

            let mut t = line.split_whitespace();
            let k = t.next();
            let n = t.next();
            let v = t.next();
            let h = t.next();
            let (k, n, v, h) = match (k, n, v, h) {
                (Some(a), Some(b), Some(c), Some(d)) => (a, b, c, d),
                _ => continue,
            };

            if k == kind && n == name {
                out.push_str(&format!("{} {} {} {}\n", kind, name, version, hash));
                written = true;
                continue;
            }

            out.push_str(&format!("{} {} {} {}\n", k, n, v, h));
        }
    }

    if !written {
        out.push_str(&format!("{} {} {} {}\n", kind, name, version, hash));
    }

    if fs::write(&tmp, out.as_bytes()).is_err() {
        return Err(format!("cannot write {}", tmp));
    }

    if fs::rename(&tmp, lock).is_err() {
        return Err(format!("cannot update {}", lock));
    }

    Ok(())
}
