use crate::model::Toolchain;
use crate::sys;
use crate::toml::Toml;
use loom_core::proc;

struct Preset {
    name: &'static str,
    family: &'static str,
    cc: &'static str,
    cxx: &'static str,
    as_: &'static str,
    ar: &'static str,
    objext: &'static str,
    binext: &'static str,
    libext: &'static str,
    dllpre: &'static str,
    dllext: &'static str,
    soflag: &'static str,
}

const PRESETS: [Preset; 9] = [
    Preset {
        name: "host",
        family: "gnu",
        cc: "cc",
        cxx: "c++",
        as_: "nasm",
        ar: "ar",
        objext: ".o",
        binext: "",
        libext: ".a",
        dllpre: "lib",
        dllext: ".so",
        soflag: "-shared",
    },
    Preset {
        name: "linux",
        family: "gnu",
        cc: "cc",
        cxx: "c++",
        as_: "nasm",
        ar: "ar",
        objext: ".o",
        binext: "",
        libext: ".a",
        dllpre: "lib",
        dllext: ".so",
        soflag: "-shared",
    },
    Preset {
        name: "gcc",
        family: "gnu",
        cc: "gcc",
        cxx: "g++",
        as_: "nasm",
        ar: "ar",
        objext: ".o",
        binext: "",
        libext: ".a",
        dllpre: "lib",
        dllext: ".so",
        soflag: "-shared",
    },
    Preset {
        name: "clang",
        family: "gnu",
        cc: "clang",
        cxx: "clang++",
        as_: "nasm",
        ar: "ar",
        objext: ".o",
        binext: "",
        libext: ".a",
        dllpre: "lib",
        dllext: ".so",
        soflag: "-shared",
    },
    Preset {
        name: "macos",
        family: "gnu",
        cc: "cc",
        cxx: "c++",
        as_: "nasm",
        ar: "ar",
        objext: ".o",
        binext: "",
        libext: ".a",
        dllpre: "lib",
        dllext: ".dylib",
        soflag: "-dynamiclib",
    },
    Preset {
        name: "armcc",
        family: "armcc",
        cc: "armcc",
        cxx: "armcc",
        as_: "armasm",
        ar: "armar",
        objext: ".o",
        binext: "",
        libext: ".lib",
        dllpre: "",
        dllext: ".dll",
        soflag: "",
    },
    Preset {
        name: "iar",
        family: "iar",
        cc: "iccarm",
        cxx: "iccarm",
        as_: "iasm",
        ar: "iarchive",
        objext: ".o",
        binext: "",
        libext: ".a",
        dllpre: "",
        dllext: ".dll",
        soflag: "",
    },
    Preset {
        name: "mingw",
        family: "gnu",
        cc: "x86_64-w64-mingw32-gcc",
        cxx: "x86_64-w64-mingw32-g++",
        as_: "x86_64-w64-mingw32-nasm",
        ar: "x86_64-w64-mingw32-ar",
        objext: ".obj",
        binext: ".exe",
        libext: ".a",
        dllpre: "lib",
        dllext: ".dll",
        soflag: "-shared",
    },
    Preset {
        name: "msvc",
        family: "msvc",
        cc: "cl",
        cxx: "cl",
        as_: "nasm",
        ar: "lib",
        objext: ".obj",
        binext: ".exe",
        libext: ".lib",
        dllpre: "",
        dllext: ".dll",
        soflag: "-shared",
    },
];

const AUTO: [&str; 4] = ["gcc", "clang", "tcc", "msvc"];

#[cfg(windows)]
const DIR_SEP: &str = "\\";

#[cfg(not(windows))]
const DIR_SEP: &str = "/";

fn preset_of(name: &str) -> Option<&'static Preset> {
    if name.is_empty() {
        return None;
    }
    PRESETS.iter().find(|p| p.name == name)
}

fn gcc_preset() -> &'static Preset {
    match preset_of("gcc") {
        Some(p) => p,
        None => &PRESETS[2],
    }
}

fn exec_ok(path: &str) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        match std::fs::metadata(path) {
            Ok(m) => (m.permissions().mode() & 0o111) != 0,
            Err(_) => false,
        }
    }
    #[cfg(not(unix))]
    {
        std::fs::metadata(path).is_ok()
    }
}

fn dir_len(dir: &str) -> usize {
    let b = dir.as_bytes();
    let mut n = b.len();
    while n > 1 && (b[n - 1] == b'/' || b[n - 1] == b'\\') {
        if (n == 3 && b[1] == b':') || b[n - 2] == b':' {
            break;
        }
        n -= 1;
    }
    n
}

#[cfg(windows)]
fn has_ext(prog: &str) -> bool {
    let base = match prog.rfind(|c| c == '/' || c == '\\') {
        Some(i) => &prog[i + 1..],
        None => prog,
    };
    base.contains('.')
}

fn exec_try(dir: &str, prog: &str) -> bool {
    let n = dir_len(dir);
    let head = &dir[..n];
    let sep = if n != 0 && !head.ends_with('/') && !head.ends_with('\\') {
        DIR_SEP
    } else {
        ""
    };
    if exec_ok(&format!("{}{}{}", head, sep, prog)) {
        return true;
    }
    #[cfg(windows)]
    {
        if !has_ext(prog) && exec_ok(&format!("{}{}{}.exe", head, sep, prog)) {
            return true;
        }
    }
    false
}

fn on_path(prog: &str) -> bool {
    if prog.contains('/') || prog.contains('\\') {
        return exec_try("", prog);
    }
    let path = match std::env::var("PATH") {
        Ok(v) => v,
        Err(_) => return false,
    };
    for dir in sys::path_dirs(&path) {
        if exec_try(dir, prog) {
            return true;
        }
    }
    false
}

fn add_env(tc: &mut Toolchain, s: &str) {
    tc.env.push(s.to_string());
}

fn env_lookup<'a>(items: &'a [String], key: &str) -> Option<&'a str> {
    for it in items {
        if let Some(rest) = it.strip_prefix(key) {
            if rest.as_bytes().first() == Some(&b'=') {
                return Some(&rest[1..]);
            }
        }
    }
    None
}

fn tc_file_exists(path: &str) -> bool {
    std::fs::metadata(path).is_ok()
}

fn cl_in(dir: &str) -> Option<String> {
    if dir.is_empty() {
        return None;
    }
    let s = format!("{}\\cl.exe", dir);
    if tc_file_exists(&s) {
        Some(s)
    } else {
        None
    }
}

fn find_cl(items: &[String], arch: &str) -> Option<String> {
    if let Some(vcdir) = env_lookup(items, "VCToolsInstallDir") {
        if !vcdir.is_empty() {
            let l = vcdir
                .trim_end_matches(|c| c == '\\' || c == '/')
                .len();
            let s = format!("{}\\bin\\Host{}\\{}\\cl.exe", &vcdir[..l], arch, arch);
            if tc_file_exists(&s) {
                return Some(s);
            }
        }
    }
    if let Some(p) = env_lookup(items, "PATH") {
        if !p.is_empty() {
            for e in p.split(';') {
                if let Some(s) = cl_in(e) {
                    return Some(s);
                }
            }
        }
    }
    None
}

fn msvc_env(tc: &mut Toolchain, arch: &str, want: Option<&str>) -> Result<Option<String>, ()> {
    let install = match vs_install() {
        Some(v) => v,
        None => return Err(()),
    };
    let raw = match vs_env_capture(&install, arch, want) {
        Some(v) => v,
        None => return Err(()),
    };
    let items = vs_env_split(&raw);
    if items.is_empty() {
        return Err(());
    }
    match env_lookup(&items, "INCLUDE") {
        Some(v) if !v.is_empty() => {}
        _ => return Err(()),
    }
    for it in &items {
        let s = it.clone();
        add_env(tc, &s);
    }
    Ok(find_cl(&items, arch))
}

fn msvc_fill(tc: &mut Toolchain, arch: &str, want: Option<&str>) -> Result<(), String> {
    let cl = match msvc_env(tc, arch, want) {
        Ok(v) => v,
        Err(_) => {
            return Err("MSVC environment could not be loaded; vcvarsall.bat was not found or failed to run"
                .to_string())
        }
    };
    let cl = match cl {
        Some(v) => v,
        None => {
            return Err("cl.exe was not found in the Visual Studio environment; check that the MSVC build tools are installed"
                .to_string())
        }
    };
    tc.cc = cl.clone();
    tc.cxx = cl.clone();
    tc.ld = cl;
    tc.ar = "lib".to_string();
    Ok(())
}

fn probe_tmpdir() -> String {
    #[cfg(windows)]
    {
        if let Ok(t) = std::env::var("TEMP") {
            if !t.is_empty() {
                return t;
            }
        }
        if let Ok(t) = std::env::var("TMP") {
            if !t.is_empty() {
                return t;
            }
        }
        ".".to_string()
    }
    #[cfg(not(windows))]
    {
        if let Ok(t) = std::env::var("TMPDIR") {
            if !t.is_empty() {
                return t;
            }
        }
        "/tmp".to_string()
    }
}

pub fn probe_triple(cc: &str) -> Result<String, String> {
    let log = format!("{}/heddle-triple.log", probe_tmpdir());
    let argv = vec![cc.to_string(), "-dumpmachine".to_string()];
    let fail = || format!("cannot determine target triple of '{}'", cc);
    let r = match proc::run(&argv, None, &log, &[]) {
        Some(r) => r,
        None => return Err(fail()),
    };
    if r.exit_code != 0 {
        let _ = std::fs::remove_file(&log);
        return Err(fail());
    }
    let data = match std::fs::read(&log) {
        Ok(d) => d,
        Err(_) => return Err(fail()),
    };
    let _ = std::fs::remove_file(&log);
    let line = match data.iter().position(|&c| c == b'\n') {
        Some(i) => &data[..i],
        None => &data[..],
    };
    let mut end = line.len();
    while end > 0 && line[end - 1] == b'\r' {
        end -= 1;
    }
    if end == 0 {
        return Err(fail());
    }
    Ok(String::from_utf8_lossy(&line[..end]).to_string())
}

pub fn triple_needs_msvc(triple: &str) -> bool {
    if triple.is_empty() {
        return false;
    }
    if !triple.contains("windows") {
        return false;
    }
    triple.contains("msvc")
}

fn maybe_load_msvc_env(tc: &mut Toolchain, arch: &str) -> Result<(), String> {
    if tc.cc.is_empty() {
        return Ok(());
    }
    if tc.family == "msvc" {
        return Ok(());
    }
    let cc = tc.cc.clone();
    let triple = match probe_triple(&cc) {
        Ok(t) => t,
        Err(_) => return Ok(()),
    };
    if !triple_needs_msvc(&triple) {
        return Ok(());
    }
    if msvc_env(tc, arch, None).is_err() {
        return Err(format!(
            "compiler '{}' targets the MSVC ABI ({}) but the Visual Studio environment could not be loaded; vcvarsall.bat was not found or failed to run",
            cc, triple
        ));
    }
    Ok(())
}

pub fn auto_count() -> usize {
    AUTO.len()
}

pub fn auto_at(i: usize) -> String {
    match AUTO.get(i) {
        Some(s) => s.to_string(),
        None => String::new(),
    }
}

pub fn probe(preset: &str) -> bool {
    let p = match preset_of(preset) {
        Some(p) => p,
        None => return false,
    };
    if preset == "msvc" {
        return vs_install().is_some();
    }
    on_path(p.cc)
}

pub fn preset_cc(preset: &str) -> String {
    match preset_of(preset) {
        Some(p) => p.cc.to_string(),
        None => String::new(),
    }
}

fn or_default<'a>(t: &'a Toml, sect: &str, key: &str, fallback: &'a str) -> &'a str {
    match t.str_of(sect, key) {
        Some(v) => v,
        None => fallback,
    }
}

fn add_flags(t: &Toml, sect: &str, key: &str, out: &mut Vec<String>) {
    let mut i = 0usize;
    while let Some(v) = t.arr(sect, key, i) {
        out.push(v.to_string());
        i += 1;
    }
}

fn arch_sel_flag(f: &str) -> bool {
    if !f.starts_with('-') {
        return false;
    }
    for p in ["-march=", "-mabi=", "-mcpu=", "-mtune="] {
        if f.starts_with(p) {
            return true;
        }
    }
    matches!(f, "-m32" | "-m64" | "-m16" | "-mx32")
}

fn add_arch_ldflags(tc: &mut Toolchain, tok: &str) {
    if !arch_sel_flag(tok) {
        return;
    }
    if !(tc.family == "gnu" || tc.family == "gcc" || tc.family == "clang") {
        return;
    }
    if tc.ldflags.iter().any(|x| x == tok) {
        return;
    }
    tc.ldflags.push(tok.to_string());
}

fn is_auto(name: &str) -> bool {
    name.is_empty() || name == "auto" || name == "native"
}

fn read_flags(dir: &str, tc: &mut Toolchain, user: &str) {
    let path = format!("{}/heddle.toml", dir);
    let t = match Toml::parse(&path) {
        Ok(t) => t,
        Err(_) => return,
    };
    let sect = format!("toolchain.{}", user);
    if let Some(cc) = t.str_of(&sect, "cc") {
        tc.cc = cc.to_string();
    }
    if let Some(a) = t.str_of(&sect, "as") {
        tc.as_ = a.to_string();
    }
    add_flags(&t, &sect, "cflags", &mut tc.cflags);
}

#[cfg(windows)]
fn host_fix(tc: &mut Toolchain) {
    if tc.family == "gnu" {
        if tc.binext.is_empty() {
            tc.binext = ".exe".to_string();
        }
        if tc.soflag == "-shared" && tc.dllpre == "lib" {
            tc.dllpre = String::new();
        }
    }
}

#[cfg(not(windows))]
fn host_fix(_tc: &mut Toolchain) {}

fn load_preset(tc: &mut Toolchain, p: &Preset) {
    *tc = Toolchain::default();
    tc.name = p.name.to_string();
    tc.family = p.family.to_string();
    tc.cc = p.cc.to_string();
    tc.cxx = p.cxx.to_string();
    tc.as_ = p.as_.to_string();
    tc.ar = p.ar.to_string();
    tc.ld = p.cc.to_string();
    tc.objext = p.objext.to_string();
    tc.binext = p.binext.to_string();
    tc.libext = p.libext.to_string();
    tc.dllpre = p.dllpre.to_string();
    tc.dllext = p.dllext.to_string();
    tc.soflag = p.soflag.to_string();
    tc.platform = p.name.to_string();
    host_fix(tc);
}

fn apply_auto(
    tc: &mut Toolchain,
    dir: &str,
    user: &str,
    arch: &str,
) -> Result<(), Option<String>> {
    for name in AUTO.iter() {
        let p = match preset_of(name) {
            Some(p) => p,
            None => continue,
        };
        if !probe(name) {
            continue;
        }
        load_preset(tc, p);
        if p.name == "msvc" {
            if let Err(e) = msvc_fill(tc, arch, None) {
                return Err(Some(e));
            }
        } else if let Err(e) = maybe_load_msvc_env(tc, arch) {
            return Err(Some(e));
        }
        read_flags(dir, tc, user);
        return Ok(());
    }
    Err(None)
}

fn msvc_arch(arch: &str) -> &str {
    if arch.is_empty() {
        return "x64";
    }
    match arch {
        "x86_64" | "amd64" => "x64",
        "i686" | "i386" | "x86" => "x86",
        "aarch64" | "arm64" => "arm64",
        "arm" => "arm",
        _ => {
            if arch.starts_with("armv") || arch.starts_with("cortex") {
                "arm"
            } else {
                arch
            }
        }
    }
}

fn apply_prefix(prefix: &str, tool: &str) -> String {
    if prefix.is_empty() || tool.is_empty() {
        return tool.to_string();
    }
    if tool.contains('/') || tool.contains('\\') {
        return tool.to_string();
    }
    if tool.starts_with(prefix) {
        return tool.to_string();
    }
    format!("{}{}", prefix, tool)
}

fn add_sysroot(tc: &mut Toolchain, sysroot: Option<&str>) {
    if let Some(sr) = sysroot {
        if !sr.is_empty() {
            tc.cflags.push("--sysroot".to_string());
            tc.cflags.push(sr.to_string());
        }
    }
}

fn add_extra(tc: &mut Toolchain, extra_cflags: &str) {
    if extra_cflags.is_empty() {
        return;
    }
    for tok in extra_cflags.split(|c: char| c == ' ' || c == '\t') {
        if tok.is_empty() {
            continue;
        }
        tc.cflags.push(tok.to_string());
        add_arch_ldflags(tc, tok);
    }
}

fn overlay(
    tc: &mut Toolchain,
    prefix: Option<&str>,
    sysroot: Option<&str>,
    extra_cflags: &str,
) {
    if let Some(pfx) = prefix {
        if !pfx.is_empty() {
            tc.binext = String::new();
            tc.dllpre = "lib".to_string();
            tc.cc = apply_prefix(pfx, &tc.cc);
            tc.cxx = apply_prefix(pfx, &tc.cxx);
            tc.ar = apply_prefix(pfx, &tc.ar);
            tc.ld = apply_prefix(pfx, &tc.ld);
            let mut buf = format!("cross-{}", pfx);
            if buf.ends_with('-') || buf.ends_with('_') {
                buf.pop();
            }
            tc.platform = buf;
            add_sysroot(tc, sysroot);
        }
    }
    add_extra(tc, extra_cflags);
}

pub fn load_ex(
    tc: &mut Toolchain,
    root: &str,
    name: &str,
    prefix: Option<&str>,
    sysroot: Option<&str>,
    extra_cflags: &str,
    arch: &str,
) -> Result<(), String> {
    let arch_msvc = msvc_arch(arch).to_string();
    *tc = Toolchain::default();

    if is_auto(name) {
        if let Some(pfx) = prefix {
            if !pfx.is_empty() {
                let cc = format!("{}gcc", pfx);
                if on_path(&cc) {
                    load_preset(tc, gcc_preset());
                    tc.name = pfx.to_string();
                    tc.cc = cc.clone();
                    tc.cxx = apply_prefix(pfx, "g++");
                    tc.ar = apply_prefix(pfx, "ar");
                    tc.ld = cc;
                    read_flags(root, tc, "host");
                    overlay(tc, prefix, sysroot, extra_cflags);
                    return Ok(());
                }
            }
        }
        match apply_auto(tc, root, "host", &arch_msvc) {
            Ok(()) => {
                overlay(tc, prefix, sysroot, extra_cflags);
                return Ok(());
            }
            Err(Some(e)) => return Err(e),
            Err(None) => {
                return Err(
                    "no C compiler found on PATH; set [build] toolchain or -t <name>".to_string(),
                )
            }
        }
    }

    let path = format!("{}/heddle.toml", root);
    let t = match Toml::parse(&path) {
        Ok(t) => t,
        Err(_) => Toml::new(),
    };
    let sect = format!("toolchain.{}", name);
    let base = or_default(&t, &sect, "based_on", name);
    let p = match preset_of(base) {
        Some(p) => p,
        None => return Err(format!("unknown toolchain '{}'", base)),
    };

    tc.name = name.to_string();
    tc.family = or_default(&t, &sect, "family", p.family).to_string();
    tc.cc = or_default(&t, &sect, "cc", p.cc).to_string();
    tc.cxx = or_default(&t, &sect, "cxx", p.cxx).to_string();
    tc.ar = or_default(&t, &sect, "ar", p.ar).to_string();
    tc.ld = or_default(&t, &sect, "ld", p.cc).to_string();
    tc.as_ = or_default(&t, &sect, "as", p.as_).to_string();
    tc.objext = or_default(&t, &sect, "objext", p.objext).to_string();
    tc.binext = or_default(&t, &sect, "binext", p.binext).to_string();
    tc.libext = or_default(&t, &sect, "libext", p.libext).to_string();
    tc.dllpre = or_default(&t, &sect, "dllpre", p.dllpre).to_string();
    tc.dllext = or_default(&t, &sect, "dllext", p.dllext).to_string();
    tc.soflag = or_default(&t, &sect, "soflag", p.soflag).to_string();
    tc.platform = or_default(&t, &sect, "platform", p.name).to_string();

    if p.name == "msvc" {
        let a = or_default(&t, &sect, "arch", &arch_msvc).to_string();
        let want = t.str_of(&sect, "toolset").map(|v| v.to_string());
        msvc_fill(tc, &a, want.as_deref())?;
    }

    add_flags(&t, &sect, "cflags", &mut tc.cflags);
    add_flags(&t, &sect, "ldflags", &mut tc.ldflags);

    if p.name != "msvc" {
        maybe_load_msvc_env(tc, &arch_msvc)?;
    }

    overlay(tc, prefix, sysroot, extra_cflags);
    Ok(())
}

pub fn load_star(
    tc: &mut Toolchain,
    name: &str,
    based: Option<&str>,
    cc: Option<&str>,
    family: Option<&str>,
    prefix: Option<&str>,
    sysroot: Option<&str>,
    extra_cflags: &str,
    arch: &str,
) -> Result<(), String> {
    *tc = Toolchain::default();

    let mut p = match based {
        Some(b) if !b.is_empty() => preset_of(b),
        _ => None,
    };
    if p.is_none() {
        if let Some(f) = family {
            if !f.is_empty() {
                p = preset_of(f);
            }
        }
    }
    if p.is_none() {
        p = preset_of(name);
    }
    let p = match p {
        Some(p) => p,
        None => gcc_preset(),
    };

    let ccv = match cc {
        Some(c) if !c.is_empty() => Some(c),
        _ => None,
    };

    load_preset(tc, p);
    tc.name = name.to_string();
    tc.family = match family {
        Some(f) if !f.is_empty() => f.to_string(),
        _ => p.family.to_string(),
    };
    tc.cc = match ccv {
        Some(c) => c.to_string(),
        None => p.cc.to_string(),
    };
    tc.cxx = match ccv {
        Some(c) => apply_prefix("", c),
        None => p.cxx.to_string(),
    };
    tc.ar = p.ar.to_string();
    tc.ld = tc.cc.clone();

    if let Some(c) = ccv {
        if !p.cxx.is_empty() {
            match c.rfind('-') {
                Some(d) if c.contains("gcc") => {
                    tc.cxx = format!("{}-g++", &c[..d]);
                }
                _ => {
                    tc.cxx = p.cxx.to_string();
                }
            }
        }
    }

    if let Some(pfx) = prefix {
        if !pfx.is_empty() {
            tc.cc = apply_prefix(pfx, &tc.cc);
            tc.cxx = apply_prefix(pfx, &tc.cxx);
            tc.ar = apply_prefix(pfx, &tc.ar);
            tc.ld = apply_prefix(pfx, &tc.ld);
            tc.platform = p.name.to_string();
            add_sysroot(tc, sysroot);
        }
    }

    add_extra(tc, extra_cflags);

    if tc.family == "msvc" {
        return msvc_fill(tc, msvc_arch(arch), None);
    }
    maybe_load_msvc_env(tc, msvc_arch(arch))
}

pub fn tool(tc: &Toolchain, name: &str) -> String {
    match name {
        "cc" => tc.cc.clone(),
        "c++" => tc.cxx.clone(),
        "nasm" => tc.as_.clone(),
        "ar" => tc.ar.clone(),
        "ld" => tc.ld.clone(),
        _ => name.to_string(),
    }
}

pub fn tool_ok(_tc: &Toolchain, tool: &str) -> bool {
    if tool.is_empty() {
        return false;
    }
    if std::fs::metadata(tool).is_ok() {
        return true;
    }
    if tool.contains('/') || tool.contains('\\') {
        return false;
    }
    let path = match std::env::var("PATH") {
        Ok(v) => v,
        Err(_) => return false,
    };
    for d in sys::path_dirs(&path) {
        if exec_try(d, tool) {
            return true;
        }
    }
    false
}

#[cfg(windows)]
fn run_capture(argv: &[String]) -> Option<String> {
    let log = format!("{}/heddle-vs.log", probe_tmpdir());
    let r = match proc::run(argv, None, &log, &[]) {
        Some(r) => r,
        None => {
            let _ = std::fs::remove_file(&log);
            return None;
        }
    };
    let data = std::fs::read(&log).ok();
    let _ = std::fs::remove_file(&log);
    if r.exit_code != 0 {
        return None;
    }
    let d = data?;
    let mut out = String::from_utf8_lossy(&d).to_string();
    let trimmed = out.trim_end_matches(|c: char| c == '\n' || c == '\r').len();
    out.truncate(trimmed);
    Some(out)
}

#[cfg(windows)]
fn vs_install() -> Option<String> {
    for key in ["ProgramFiles(x86)", "ProgramFiles"] {
        if let Ok(root) = std::env::var(key) {
            let vs = format!(
                "{}\\Microsoft Visual Studio\\Installer\\vswhere.exe",
                root
            );
            if tc_file_exists(&vs) {
                let argv = vec![
                    vs,
                    "-latest".to_string(),
                    "-products".to_string(),
                    "*".to_string(),
                    "-prerelease".to_string(),
                    "-requires".to_string(),
                    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64".to_string(),
                    "-property".to_string(),
                    "installationPath".to_string(),
                ];
                return run_capture(&argv);
            }
        }
    }
    None
}

#[cfg(windows)]
fn vs_env_capture(install: &str, arch: &str, ver: Option<&str>) -> Option<String> {
    let script = format!("{}\\VC\\Auxiliary\\Build\\vcvarsall.bat", install);
    if !tc_file_exists(&script) {
        return None;
    }
    let a = if arch.is_empty() { "x64" } else { arch };
    let arg = match ver {
        Some(v) if !v.is_empty() => format!("{} -vcvars_ver={}", a, v),
        _ => a.to_string(),
    };
    let bat = format!("{}/heddle-vcvars.bat", probe_tmpdir());
    let body = format!(
        "@echo off\r\ncall \"{}\" {} >nul 2>nul\r\nif errorlevel 1 exit /b 1\r\nset\r\n",
        script, arg
    );
    if std::fs::write(&bat, body.as_bytes()).is_err() {
        return None;
    }
    let argv = vec![
        "cmd".to_string(),
        "/d".to_string(),
        "/s".to_string(),
        "/c".to_string(),
        bat,
    ];
    let out = run_capture(&argv);
    let _ = std::fs::remove_file(format!("{}/heddle-vcvars.bat", probe_tmpdir()));
    let out = out?;
    if out.is_empty() {
        return None;
    }
    Some(out)
}

#[cfg(windows)]
fn vs_env_split(raw: &str) -> Vec<String> {
    let mut items = Vec::new();
    for line in raw.split('\n') {
        let s = line.trim_end_matches('\r');
        if !s.is_empty() {
            items.push(s.to_string());
        }
    }
    items
}

#[cfg(not(windows))]
fn vs_install() -> Option<String> {
    None
}

#[cfg(not(windows))]
fn vs_env_capture(_install: &str, _arch: &str, _ver: Option<&str>) -> Option<String> {
    None
}

#[cfg(not(windows))]
fn vs_env_split(_raw: &str) -> Vec<String> {
    Vec::new()
}
