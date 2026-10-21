use crate::lang;
use crate::link::{self, LinkReq};
use crate::model::{Project, PkgSource, Target, TargetType};
use crate::project;
use crate::sys;
use crate::toolchain;
use std::collections::HashMap;

const CMD_MAX: usize = 8000;

#[derive(Default)]
struct Step {
    cmd: String,
    outs: Vec<String>,
    ins: Vec<String>,
    deps: Vec<usize>,
}

#[derive(Default)]
struct Plan {
    steps: Vec<Step>,
}

impl Plan {
    fn add(&mut self, cmd: String) -> usize {
        self.steps.push(Step {
            cmd,
            ..Default::default()
        });
        self.steps.len() - 1
    }

    fn step_out(&mut self, id: usize, path: &str) {
        self.steps[id].outs.push(path.to_string());
    }

    fn step_in(&mut self, id: usize, path: &str) {
        self.steps[id].ins.push(path.to_string());
    }

    fn step_dep(&mut self, id: usize, dep: usize) {
        if !self.steps[id].deps.contains(&dep) {
            self.steps[id].deps.push(dep);
        }
    }
}

static mut SELF_NAME: Option<String> = None;

pub fn set_self(argv0: &str) {
    let v = if argv0.is_empty() { "heddle" } else { argv0 };
    unsafe {
        SELF_NAME = Some(v.to_string());
    }
}

fn self_name() -> String {
    unsafe {
        match &*std::ptr::addr_of!(SELF_NAME) {
            Some(s) => s.clone(),
            None => "heddle".to_string(),
        }
    }
}

fn tc_is_msvc(p: &Project) -> bool {
    p.tc.family == "msvc"
}

fn is_supported(src: &str) -> bool {
    lang::for_path(src).is_some()
}

fn have_src(t: &Target, path: &str) -> bool {
    t.src.iter().any(|s| s == path)
}

fn owner_of<'a>(p: &'a Project, skip: &Target, path: &str) -> Option<&'a Target> {
    p.targets
        .iter()
        .find(|t| !std::ptr::eq(*t, skip) && have_src(t, path))
}

fn cycle_at(p: &Project, i: usize, state: &mut [u8]) -> Result<(), String> {
    if state[i] == 1 {
        return Err(format!(
            "dependency cycle involving target '{}'",
            p.targets[i].name
        ));
    }
    if state[i] == 2 {
        return Ok(());
    }
    state[i] = 1;
    let t = &p.targets[i];
    for d in &t.deps {
        if let Some(j) = p.targets.iter().position(|x| &x.name == d) {
            cycle_at(p, j, state)?;
        }
    }
    state[i] = 2;
    Ok(())
}

pub fn deps_ready(p: &Project) -> Result<(), String> {
    for s in &p.pkg.deps {
        if s.source == PkgSource::PkgConfig {
            continue;
        }
        if s.store_path.is_empty() || sys::stat(&s.store_path).is_none() {
            return Err(format!(
                "dependency '{}@{}' is not installed; run 'heddle tool install'",
                s.name, s.version
            ));
        }
    }
    Ok(())
}

pub fn project_check(p: &Project) -> Result<(), String> {
    for t in &p.targets {
        for d in &t.deps {
            let dt = match project::target(p, d) {
                Some(x) => x,
                None => {
                    return Err(format!(
                        "target '{}': unknown dependency '{}'",
                        t.name, d
                    ))
                }
            };
            if dt.kind == TargetType::Exe && t.kind != TargetType::Custom {
                return Err(format!(
                    "target '{}': cannot depend on executable '{}'",
                    t.name, dt.name
                ));
            }
            if std::ptr::eq(dt, t) {
                return Err(format!("target '{}': depends on itself", t.name));
            }
        }
        for src in &t.src {
            if src.contains("..") {
                return Err(format!("target '{}': path escapes project '{}'", t.name, src));
            }
            if !is_supported(src) {
                return Err(format!("target '{}': unsupported source '{}'", t.name, src));
            }
            if let Some(o) = owner_of(p, t, src) {
                return Err(format!(
                    "target '{}': source '{}' also used by '{}'",
                    t.name, src, o.name
                ));
            }
            if sys::stat(src).is_none() {
                return Err(format!("target '{}': missing source '{}'", t.name, src));
            }
        }
        for inc in &t.inc {
            if sys::stat(inc).is_none() {
                return Err(format!(
                    "target '{}': missing include dir '{}'",
                    t.name, inc
                ));
            }
        }
    }
    let mut state = vec![0u8; p.targets.len()];
    for i in 0..p.targets.len() {
        cycle_at(p, i, &mut state)?;
    }
    Ok(())
}

fn target_index(p: &Project, name: &str) -> Option<usize> {
    p.targets.iter().position(|t| t.name == name)
}

fn mark_closure(p: &Project, i: usize, want: &mut [u8]) {
    if want[i] != 0 {
        return;
    }
    want[i] = 1;
    for d in &p.targets[i].deps {
        if let Some(j) = target_index(p, d) {
            mark_closure(p, j, want);
        }
    }
}

fn object_of(p: &Project, t: &Target, src: &str) -> String {
    let base = sys::base_name(src);
    let stem = match base.rfind('.') {
        Some(i) if i > 0 => &base[..i],
        _ => base,
    };
    let ext = lang::for_path(src)
        .and_then(|l| l.outext)
        .unwrap_or_else(|| p.tc.objext.clone());
    let name = format!("{}_{}{}", t.name, stem, ext);
    format!("{}/{}", p.build_dir, name)
}

pub fn artifact(p: &Project, t: &Target) -> Option<String> {
    if let Some(o) = &t.out {
        return Some(project::path(&p.root, o));
    }
    let mut pre = String::new();
    let mut ext = p.tc.binext.clone();
    if t.kind == TargetType::StaticLib {
        pre = "lib".to_string();
        ext = p.tc.libext.clone();
    } else if t.kind == TargetType::SharedLib {
        pre = p.tc.dllpre.clone();
        ext = p.tc.dllext.clone();
    }
    let name = format!("{}{}{}", pre, t.name, ext);
    Some(format!("{}/{}", p.build_dir, name))
}

fn asm_format(p: &Project) -> &'static str {
    let arch = p.pkg.target.arch.as_str();
    let wide = arch == "x86_64"
        || arch == "aarch64"
        || arch == "riscv64"
        || arch == "powerpc";
    if let Some(pre) = &p.target_prefix {
        if !pre.is_empty() {
            return if wide { "elf64" } else { "elf32" };
        }
    }
    if cfg!(windows) {
        return if wide { "win64" } else { "win32" };
    }
    if cfg!(target_os = "macos") {
        return if wide { "macho64" } else { "macho32" };
    }
    if wide {
        "elf64"
    } else {
        "elf32"
    }
}

fn target_incs(p: &Project, t: &Target) -> Vec<String> {
    let msvc = tc_is_msvc(p);
    let mut out = Vec::new();
    for i in &t.inc {
        out.push(if msvc {
            format!("/I{}", i)
        } else {
            format!("-I{}", i)
        });
    }
    for s in &p.pkg.deps {
        if s.source == PkgSource::PkgConfig {
            out.extend(s.cflags.iter().cloned());
            continue;
        }
        if s.store_path.is_empty() {
            continue;
        }
        let inc = project::path(&s.store_path, "include");
        if let Some(st) = sys::stat(&inc) {
            if st.is_dir {
                out.push(if msvc {
                    format!("/I{}", inc)
                } else {
                    format!("-I{}", inc)
                });
            }
        }
    }
    out
}

fn target_flags(p: &Project, t: &Target) -> Vec<String> {
    let mut out = p.tc.cflags.clone();
    out.extend(t.cflags.iter().cloned());
    out
}

fn put_dep_ldflags(p: &Project, out: &mut Vec<String>) {
    for s in &p.pkg.deps {
        if s.source == PkgSource::PkgConfig {
            out.extend(s.ldflags.iter().cloned());
            continue;
        }
        if s.store_path.is_empty() || s.recipe {
            continue;
        }
        let lib = project::path(&s.store_path, "lib");
        match sys::stat(&lib) {
            Some(st) if st.is_dir => {}
            _ => continue,
        }
        out.push(format!("-L{}", lib));
        let meta = project::path(&s.store_path, ".heddle-pkg");
        if let Some(text) = sys::read_to_string(&meta) {
            for line in text.lines() {
                let l = line.trim_end_matches(['\r', '\n']);
                if l.is_empty() {
                    continue;
                }
                out.push(format!("-l{}", l));
            }
        } else {
            out.push(format!("-l{}", s.name));
        }
    }
}

fn subst_arg(arg: &str, src: &str, out: &str, format: &str, root: &str) -> String {
    let mut s = String::new();
    let mut rest = arg;
    while !rest.is_empty() {
        if let Some(r) = rest.strip_prefix("{src}") {
            s.push_str(src);
            rest = r;
        } else if let Some(r) = rest.strip_prefix("{out}") {
            s.push_str(out);
            rest = r;
        } else if let Some(r) = rest.strip_prefix("{format}") {
            s.push_str(format);
            rest = r;
        } else if let Some(r) = rest.strip_prefix("{root}") {
            s.push_str(root);
            rest = r;
        } else {
            let ch = rest.chars().next().unwrap();
            s.push(ch);
            rest = &rest[ch.len_utf8()..];
        }
    }
    s
}

fn lang_is_template(a: &lang::Lang) -> bool {
    a.args.iter().any(|x| {
        x.contains("{src}") || x.contains("{out}") || x.contains("{format}") || x.contains("{root}")
    })
}

fn build_cmd(
    p: &Project,
    t: &Target,
    lg: &lang::Lang,
    src: &str,
    out: &str,
    incs: &[String],
    flags: &[String],
    deffmt: &str,
    flags_rsp: Option<&str>,
) -> Option<String> {
    let fmt = t.format.clone().unwrap_or_else(|| deffmt.to_string());
    let msvc = tc_is_msvc(p);
    let mut cmd = toolchain::tool(&p.tc, &lg.cmd);

    if lang_is_template(lg) {
        for a in &lg.args {
            cmd.push(' ');
            cmd.push_str(&subst_arg(a, src, out, &fmt, &p.root));
        }
        return if cmd.len() < 16384 { Some(cmd) } else { None };
    }

    if msvc {
        cmd.push_str(" /c /nologo");
    } else {
        for a in &lg.args {
            cmd.push(' ');
            cmd.push_str(&subst_arg(a, src, out, &fmt, &p.root));
        }
        if lg.fmt {
            cmd.push_str(&format!(" -f {}", fmt));
        }
    }

    if lg.cflags {
        match flags_rsp {
            Some(r) => cmd.push_str(&format!(" \"@{}\"", r)),
            None => {
                for f in flags {
                    cmd.push(' ');
                    cmd.push_str(f);
                }
            }
        }
    }

    for i in incs {
        cmd.push(' ');
        cmd.push_str(i);
    }

    if msvc {
        cmd.push_str(&format!(" /I{} /Fo{} {}", p.root, out, src));
    } else {
        cmd.push_str(&format!(" -I{} -o {} {}", p.root, out, src));
    }

    if cmd.len() < 16384 {
        Some(cmd)
    } else {
        None
    }
}

fn join_flags(flags: &[String]) -> String {
    flags.join(" ")
}

fn write_rsp(path: &str, r: &LinkReq) -> bool {
    let mut s = String::new();
    for o in &r.objs {
        s.push_str(&rsp_arg(o));
    }
    for l in &r.libs {
        s.push_str(&rsp_arg(l));
    }
    for f in &r.ldflags {
        if !f.is_empty() {
            s.push_str(&rsp_arg(f));
        }
    }
    sys::write_bytes(path, s.as_bytes())
}

fn rsp_arg(s: &str) -> String {
    if s.contains(' ') || s.contains('\t') {
        format!("\"{}\"\n", s)
    } else {
        format!("{}\n", s)
    }
}

fn write_flag_rsp(path: &str, flags: &[String]) -> bool {
    let mut s = String::new();
    for f in flags {
        s.push_str(&rsp_arg(f));
    }
    sys::write_bytes(path, s.as_bytes())
}

fn ld_is_gnu(p: &Project) -> bool {
    p.tc.family.is_empty() || p.tc.family == "gnu"
}

fn libs_open_group(p: &Project, t: &Target, libs: &mut Vec<String>) {
    if !ld_is_gnu(p) || t.deps.is_empty() {
        return;
    }
    if t.whole_archive {
        libs.push("-Wl,--whole-archive".to_string());
    }
    if t.start_group {
        libs.push("-Wl,--start-group".to_string());
    }
}

fn libs_close_group(p: &Project, t: &Target, libs: &mut Vec<String>) {
    if !ld_is_gnu(p) || t.deps.is_empty() {
        return;
    }
    if t.whole_archive {
        libs.push("-Wl,--no-whole-archive".to_string());
    }
    if t.start_group {
        libs.push("-Wl,--end-group".to_string());
    }
}

fn is_compile_unit(t: &Target) -> bool {
    t.kind != TargetType::Custom && t.kind != TargetType::Raw
}

fn emit_objects(
    p: &Project,
    pl: &mut Plan,
    obj_of: &mut [Vec<usize>],
    want: &[u8],
) -> Result<(), String> {
    for i in 0..p.targets.len() {
        if want[i] == 0 {
            continue;
        }
        let t = &p.targets[i];
        if t.kind == TargetType::Custom {
            continue;
        }
        let incs = target_incs(p, t);
        let flags = target_flags(p, t);
        let mut frsp: Option<String> = None;
        if !flags.is_empty() {
            let joined = join_flags(&flags);
            if joined.len() > CMD_MAX / 2 {
                let path = format!("{}/{}.flags.rsp", p.build_dir, t.name);
                if !write_flag_rsp(&path, &flags) {
                    return Err(format!("cannot write {}", path));
                }
                frsp = Some(path);
            }
        }

        if t.kind == TargetType::Raw {
            if t.src.len() != 1 {
                return Err(format!("target '{}': raw needs exactly one source", t.name));
            }
            let lg = match lang::for_path(&t.src[0]) {
                Some(l) => l,
                None => {
                    return Err(format!(
                        "target '{}': no language for '{}'",
                        t.name, t.src[0]
                    ))
                }
            };
            let bin = artifact(p, t).ok_or_else(|| "out of memory".to_string())?;
            let cmd = build_cmd(
                p,
                t,
                &lg,
                &t.src[0],
                &bin,
                &incs,
                &flags,
                "bin",
                frsp.as_deref(),
            )
            .ok_or_else(|| format!("target '{}': command too long", t.name))?;
            let id = pl.add(cmd);
            if let Some(r) = &frsp {
                pl.step_in(id, r);
            }
            pl.step_out(id, &bin);
            obj_of[i][0] = id;
            continue;
        }

        for k in 0..t.src.len() {
            let obj = object_of(p, t, &t.src[k]);
            let lg = match lang::for_path(&t.src[k]) {
                Some(l) => l,
                None => {
                    return Err(format!(
                        "target '{}': no language for '{}'",
                        t.name, t.src[k]
                    ))
                }
            };
            let cmd = build_cmd(
                p,
                t,
                &lg,
                &t.src[k],
                &obj,
                &incs,
                &flags,
                asm_format(p),
                frsp.as_deref(),
            )
            .ok_or_else(|| format!("target '{}': command too long", t.name))?;
            let id = pl.add(cmd);
            if let Some(r) = &frsp {
                pl.step_in(id, r);
            }
            pl.step_out(id, &obj);
            obj_of[i][k] = id;
        }
    }
    Ok(())
}

fn ld_ext(family: &str) -> Option<&'static str> {
    match family {
        "iar" => Some(".icf"),
        "armcc" => Some(".sct"),
        _ => None,
    }
}

fn is_gnu_script(path: &str) -> bool {
    path.ends_with(".ld") || path.ends_with(".lds")
}

fn script_for_family(
    p: &Project,
    t: &Target,
    pl: &mut Plan,
) -> Result<(Option<String>, Option<usize>), String> {
    let fam = if p.tc.family.is_empty() {
        "gnu"
    } else {
        p.tc.family.as_str()
    };
    let script = match &t.ldscript {
        Some(s) => s.clone(),
        None => return Ok((None, None)),
    };
    let ext = ld_ext(fam);
    let ext = match ext {
        Some(e) => e,
        None => return Ok((Some(script), None)),
    };
    if !is_gnu_script(&script) {
        return Ok((Some(script), None));
    }
    let out = format!("{}/{}{}", p.build_dir, t.name, ext);
    let cmd = format!("'{}' ldconv {} {} '{}'", self_name(), script, fam, out);
    let id = pl.add(cmd);
    pl.step_out(id, &out);
    Ok((Some(out), Some(id)))
}

fn plan_graph(p: &Project, target: &str) -> Result<(Plan, Vec<Option<usize>>), String> {
    let root = match target_index(p, target) {
        Some(i) => i,
        None => return Err(format!("unknown target '{}'", target)),
    };
    let n = p.targets.len();
    let mut want = vec![0u8; n];
    mark_closure(p, root, &mut want);

    let mut obj_of: Vec<Vec<usize>> = p.targets.iter().map(|t| vec![0usize; t.src.len()]).collect();
    let mut art: Vec<Option<usize>> = vec![None; n];
    let mut pl = Plan::default();

    emit_objects(p, &mut pl, &mut obj_of, &want)?;

    let mut order: Vec<usize> = Vec::new();
    let mut done = vec![0u8; n];
    while order.len() < n {
        let mut progressed = false;
        for i in 0..n {
            if done[i] != 0 {
                continue;
            }
            let mut ready = true;
            for d in &p.targets[i].deps {
                if let Some(j) = target_index(p, d) {
                    if done[j] == 0 {
                        ready = false;
                    }
                }
            }
            if !ready {
                continue;
            }
            order.push(i);
            done[i] = 1;
            progressed = true;
        }
        if !progressed {
            break;
        }
    }

    for i in order {
        if want[i] == 0 {
            continue;
        }
        let t = &p.targets[i];
        if t.kind == TargetType::Raw {
            art[i] = Some(obj_of[i][0]);
            continue;
        }
        let out = match artifact(p, t) {
            Some(o) => o,
            None => continue,
        };
        let mut rsp: Option<String> = None;
        let mut sstep: Option<usize> = None;

        let cmd: String = if t.kind == TargetType::Custom {
            let inner = t.cmd.clone().unwrap_or_default();
            let mut s = if cfg!(windows) {
                format!("cmd /c \"{}\"", inner)
            } else {
                format!("sh -c \"{}\"", inner)
            };
            for src in &t.src {
                s.push(' ');
                s.push_str(src);
            }
            s.push_str(&format!(" > {}", out));
            s
        } else if t.kind == TargetType::StaticLib {
            let mut s = if tc_is_msvc(p) {
                format!("{} /nologo /OUT:{}", p.tc.ar, out)
            } else {
                format!("{} rcs {}", p.tc.ar, out)
            };
            for src in &t.src {
                let obj = object_of(p, t, src);
                s.push(' ');
                s.push_str(&obj);
            }
            s
        } else {
            let mut objs: Vec<String> = Vec::new();
            for src in &t.src {
                objs.push(object_of(p, t, src));
            }
            let mut libs: Vec<String> = Vec::new();
            libs_open_group(p, t, &mut libs);
            for d in &t.deps {
                if let Some(j) = target_index(p, d) {
                    if let Some(lib) = artifact(p, &p.targets[j]) {
                        libs.push(lib);
                    }
                }
            }
            libs_close_group(p, t, &mut libs);

            let mut ldf: Vec<String> = p.tc.ldflags.clone();
            ldf.extend(t.ldflags.iter().cloned());
            put_dep_ldflags(p, &mut ldf);

            let (script, sid) = script_for_family(p, t, &mut pl)?;
            sstep = sid;

            let mut req = LinkReq {
                out: out.clone(),
                objs,
                libs,
                ldflags: ldf,
                ldscript: script,
                entry: t.entry.clone(),
                shared: t.kind == TargetType::SharedLib,
                rsp: None,
            };
            if link::link_cmd_len(&p.tc, &req) > CMD_MAX {
                let path = format!("{}/{}.rsp", p.build_dir, t.name);
                if !write_rsp(&path, &req) {
                    return Err(format!("cannot write {}", path));
                }
                req.rsp = Some(path.clone());
                rsp = Some(path);
            }
            link::link_cmd(&p.tc, &req)
        };

        let id = pl.add(cmd);
        if let Some(r) = &rsp {
            pl.step_in(id, r);
        }
        if let Some(s) = sstep {
            pl.step_dep(id, s);
        }
        pl.step_out(id, &out);
        art[i] = Some(id);
    }

    for i in 0..n {
        let id = match art[i] {
            Some(x) => x,
            None => continue,
        };
        if p.targets[i].kind != TargetType::Raw {
            for k in 0..p.targets[i].src.len() {
                let o = obj_of[i][k];
                pl.step_dep(id, o);
            }
        }
        for d in &p.targets[i].deps {
            if let Some(j) = target_index(p, d) {
                if let Some(a) = art[j] {
                    pl.step_dep(id, a);
                }
            }
        }
    }

    let mut outs: HashMap<String, usize> = HashMap::new();
    for i in 0..pl.steps.len() {
        for k in 0..pl.steps[i].outs.len() {
            let o = pl.steps[i].outs[k].clone();
            if let Some(src) = outs.get(&o) {
                if *src < i {
                    pl.step_dep(i, *src);
                }
            }
        }
        for k in 0..pl.steps[i].outs.len() {
            let o = pl.steps[i].outs[k].clone();
            outs.entry(o).or_insert(i);
        }
    }

    Ok((pl, art))
}

pub fn graph(p: &Project, target: &str, path: &str) -> Result<(), String> {
    let (pl, _art) = plan_graph(p, target)?;
    let mut out = String::new();
    for (i, st) in pl.steps.iter().enumerate() {
        out.push_str(&format!("{}: {}", i, st.cmd));
        if !st.ins.is_empty() {
            out.push_str(" @");
            for k in &st.ins {
                out.push(' ');
                out.push_str(k);
            }
        }
        if !st.deps.is_empty() {
            out.push_str(" <");
            for k in &st.deps {
                out.push_str(&format!(" {}", k));
            }
        }
        out.push('\n');
    }
    if !sys::write_bytes(path, out.as_bytes()) {
        return Err(format!("cannot write {}", path));
    }
    Ok(())
}

fn abs_dir(root: &str) -> String {
    let cwd = std::env::current_dir()
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_default();
    let mut s = if root.is_empty() || root == "." {
        if cwd.is_empty() {
            ".".to_string()
        } else {
            cwd
        }
    } else if root.starts_with('/') || (root.len() > 1 && root.as_bytes()[1] == b':') {
        root.to_string()
    } else {
        format!("{}/{}", cwd, root)
    };
    s = s.replace('\\', "/");
    s
}

fn json_str(s: &str) -> String {
    let mut out = String::from("\"");
    for b in s.bytes() {
        match b {
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            b'\n' => out.push_str("\\n"),
            b'\t' => out.push_str("\\t"),
            b'\r' => out.push_str("\\r"),
            c if c < 0x20 => out.push_str(&format!("\\u{:04x}", c)),
            c => out.push(c as char),
        }
    }
    out.push('"');
    out
}

pub fn compile_db(p: &Project, target: Option<&str>, out: &str) -> Result<(), String> {
    let n = p.targets.len();
    let mut want = vec![0u8; n];
    match target {
        Some(name) => {
            let root = match target_index(p, name) {
                Some(i) => i,
                None => return Err(format!("unknown target '{}'", name)),
            };
            mark_closure(p, root, &mut want);
        }
        None => {
            for w in want.iter_mut() {
                *w = 1;
            }
        }
    }

    let mut s = String::from("[\n");
    let mut first = true;
    for i in 0..n {
        let t = &p.targets[i];
        if want[i] == 0 || !is_compile_unit(t) {
            continue;
        }
        let incs = target_incs(p, t);
        let flags = target_flags(p, t);
        for src in &t.src {
            let lg = match lang::for_path(src) {
                Some(l) => l,
                None => continue,
            };
            let obj = object_of(p, t, src);
            let cmd = match build_cmd(p, t, &lg, src, &obj, &incs, &flags, asm_format(p), None) {
                Some(c) => c,
                None => {
                    eprintln!("heddle: skipping {}: command too long", src);
                    continue;
                }
            };
            if !first {
                s.push_str(",\n");
            }
            first = false;
            let dir = abs_dir(&p.root);
            s.push_str("  {\n    \"directory\": ");
            s.push_str(&json_str(&dir));
            s.push_str(",\n    \"file\": ");
            s.push_str(&json_str(src));
            s.push_str(",\n    \"command\": ");
            s.push_str(&json_str(&cmd));
            s.push_str(",\n    \"output\": ");
            s.push_str(&json_str(&obj));
            s.push_str("\n  }");
        }
    }
    s.push_str("\n]\n");
    if !sys::write_bytes(out, s.as_bytes()) {
        return Err(format!("cannot write {}", out));
    }
    Ok(())
}
