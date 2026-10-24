use crate::emit;
use crate::install;
use crate::lang;
use crate::model::{HeddleOpts, Project, PkgManifest, TargetType};
use crate::pkg;
use crate::project;
use crate::sys;
use crate::toolchain;
use crate::toml::Toml;
use loom_core::build::{BuildEngine, BuildOpts};
use loom_core::hash::{hash_text, hash_u64, HASH_SEED};
use loom_core::proc;

fn load(o: &HeddleOpts) -> Result<Project, String> {
    if !sys::chdir(&o.root) {
        return Err(format!("cannot enter project directory '{}'", o.root));
    }
    project::set_target(o.target.as_deref());
    let p = project::load(".", o.toolchain.as_deref())?;
    emit::project_check(&p)?;
    if let Some(t) = &o.target {
        if project::target(&p, t).is_none() {
            return Err(format!("unknown target '{}'", t));
        }
    }
    Ok(p)
}

fn build_env(p: &Project) -> Vec<(String, String)> {
    let mut out: Vec<(String, String)> = Vec::new();
    for e in &p.tc.env {
        if let Some((k, v)) = e.split_once('=') {
            out.push((k.to_string(), v.to_string()));
        }
    }
    for e in pkg::env(&p.pkg) {
        if let Some((k, v)) = e.split_once('=') {
            out.push((k.to_string(), v.to_string()));
        }
    }
    out
}

fn build_target(o: &HeddleOpts, p: &Project) -> i32 {
    if let Err(e) = emit::deps_ready(p) {
        eprintln!("heddle: {}", e);
        return 1;
    }
    sys::mkpath(".heddle");
    let graph = format!(".heddle/{}.graph", o.target.clone().unwrap_or_default());
    if let Err(e) = emit::graph(p, o.target.as_deref().unwrap_or(""), &graph) {
        eprintln!("heddle: {}", e);
        return 1;
    }

    let bo = BuildOpts {
        graph_file: graph,
        cwd: None,
        logdir: ".heddle".to_string(),
        jobs: if o.jobs > 0 { o.jobs } else { 1 },
        retry: 0,
        keep_going: true,
        cache_dir: o.cache.clone(),
        remote: o.remote.clone(),
        no_cache: o.no_cache,
        env: build_env(p),
    };
    let mut e = match BuildEngine::open(bo) {
        Ok(e) => e,
        Err(err) => {
            eprintln!("heddle: {}", err);
            return 1;
        }
    };

    if o.verbose {
        let variant = pkg::variant_key(&p.pkg.target, &p.tc);
        eprintln!(
            "heddle: toolchain={} platform={} variant={}",
            p.tc.name, p.tc.platform, variant
        );
    }

    let rc = e.run();
    if o.verbose {
        eprintln!("heddle: {} ran, {} cached", e.ran, e.cached);
    }
    rc
}

pub fn heddle_check(o: &HeddleOpts) -> i32 {
    let p = match load(o) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    println!(
        "heddle: {} targets ok, toolchain={} platform={}",
        p.targets.len(),
        p.tc.name,
        p.tc.platform
    );
    for t in &p.targets {
        println!(
            "  {:<16} {:<10} src={} inc={} deps={}",
            t.name,
            t.kind.as_str(),
            t.src.len(),
            t.inc.len(),
            t.deps.len()
        );
    }
    0
}

pub fn heddle_run(o: &HeddleOpts) -> i32 {
    let p = match load(o) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    build_target(o, &p)
}

fn watch_skip_dir(name: &str) -> bool {
    name.starts_with('.')
}

fn watch_file(name: &str) -> bool {
    if name.ends_with(".h") || name.ends_with(".hpp") || name.ends_with(".hh") {
        return true;
    }
    if name.ends_with(".toml") || name.ends_with(".star") {
        return true;
    }
    lang::for_path(name).is_some()
}

fn watch_walk(dir: &str, skip: &str, acc: &mut u64, n: &mut usize) {
    let rd = match std::fs::read_dir(dir) {
        Ok(r) => r,
        Err(_) => return,
    };
    for ent in rd.flatten() {
        let name = ent.file_name().to_string_lossy().into_owned();
        if name == "." || name == ".." {
            continue;
        }
        let full = format!("{}/{}", dir, name);
        if !skip.is_empty() && full == skip {
            continue;
        }
        let st = match sys::stat(&full) {
            Some(s) => s,
            None => continue,
        };
        if st.is_dir {
            if !watch_skip_dir(&name) {
                watch_walk(&full, skip, acc, n);
            }
            continue;
        }
        if !watch_file(&name) {
            continue;
        }
        *acc = hash_u64(*acc, hash_text(HASH_SEED, &full));
        *acc = hash_u64(*acc, st.mtime_ns as u64);
        *acc = hash_u64(*acc, st.size as u64);
        *n += 1;
    }
}

fn watch_scan(p: &Project) -> u64 {
    let mut acc = HASH_SEED;
    let mut n = 0usize;
    watch_walk(&p.root, &p.build_dir, &mut acc, &mut n);
    acc
}

pub fn heddle_watch(o: &HeddleOpts) -> i32 {
    let interval = if o.interval_ms > 0 {
        o.interval_ms as u64
    } else {
        500
    };
    let mut p = match load(o) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    println!(
        "heddle: watching {} every {}ms, Ctrl-C to stop",
        p.root, interval
    );
    use std::io::Write;
    let _ = std::io::stdout().flush();
    loop {
        let rc = build_target(o, &p);
        println!(
            "heddle: {} (exit {})",
            if rc == 0 { "ok" } else { "failed" },
            rc
        );
        let _ = std::io::stdout().flush();

        let before = watch_scan(&p);
        loop {
            sys::sleep_ms(interval);
            if watch_scan(&p) != before {
                break;
            }
        }
        println!("heddle: change detected, rebuilding");
        let _ = std::io::stdout().flush();

        match load(o) {
            Ok(q) => p = q,
            Err(e) => {
                eprintln!("heddle: {}", e);
                return 1;
            }
        }
    }
}

fn artifact_path(p: &Project, t: &crate::model::Target) -> Option<String> {
    let bin = emit::artifact(p, t)?;
    if sys::stat(&bin).is_some() {
        return Some(bin);
    }
    if cfg!(windows) {
        let alt = format!("{}.exe", bin);
        if sys::stat(&alt).is_some() {
            return Some(alt);
        }
    }
    Some(bin)
}

pub fn heddle_exec(o: &HeddleOpts) -> i32 {
    let p = match load(o) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    let tname = o.target.clone().unwrap_or_default();
    let t = match project::target(&p, &tname) {
        Some(t) => t.clone(),
        None => {
            eprintln!("heddle: unknown target '{}'", tname);
            return 1;
        }
    };
    if t.kind != TargetType::Exe {
        eprintln!(
            "heddle: cannot run '{}': only executables can be run (type={})",
            t.name,
            t.kind.as_str()
        );
        return 1;
    }
    let rc = build_target(o, &p);
    if rc != 0 {
        return rc;
    }
    let bin = match artifact_path(&p, &t) {
        Some(b) => b,
        None => {
            eprintln!(
                "heddle: cannot determine artifact path for target '{}'",
                t.name
            );
            return 1;
        }
    };
    if sys::stat(&bin).is_none() {
        eprintln!("heddle: cannot find '{}'", bin);
        return 1;
    }
    if o.verbose {
        print!("heddle: run {}", bin);
        for a in &o.run_args {
            print!(" {}", a);
        }
        println!();
        use std::io::Write;
        let _ = std::io::stdout().flush();
    }

    let mut argv: Vec<String> = vec![bin.clone()];
    argv.extend(o.run_args.iter().cloned());
    let env = build_env(&p);
    match proc::run_inherit(&argv, None, &env) {
        None => {
            eprintln!("heddle: cannot run '{}'", bin);
            1
        }
        Some(r) => {
            if r.signaled {
                128 + r.signal
            } else {
                r.exit_code
            }
        }
    }
}

pub fn heddle_toolchains() -> i32 {
    println!("detected toolchains (auto uses the first match):");
    for i in 0..toolchain::auto_count() {
        let name = toolchain::auto_at(i);
        let ok = toolchain::probe(&name);
        println!(
            "  {:<8} {:<4} {}",
            name,
            if ok { "ok" } else { "-" },
            toolchain::preset_cc(&name)
        );
        if !ok {
            continue;
        }
        let triple = match toolchain::probe_triple(&toolchain::preset_cc(&name)) {
            Ok(t) => t,
            Err(_) => continue,
        };
        println!(
            "           target {}{}",
            triple,
            if toolchain::triple_needs_msvc(&triple) {
                "  (needs MSVC env, loaded on use)"
            } else {
                ""
            }
        );
    }
    0
}

fn apply_registry_env(o: &HeddleOpts) {
    if let Some(r) = &o.registry {
        if !r.is_empty() {
            std::env::set_var("HEDDLE_REGISTRY", r);
        }
    }
}

fn pkg_load(o: &HeddleOpts) -> Result<PkgManifest, String> {
    if !sys::chdir(&o.root) {
        return Err(format!("cannot enter project directory '{}'", o.root));
    }
    apply_registry_env(o);
    pkg::manifest_load(".")
}

pub fn heddle_tool_plan(o: &HeddleOpts) -> i32 {
    let m = match pkg_load(o) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    pkg::plan(&m);
    0
}

pub fn heddle_tool_install(o: &HeddleOpts) -> i32 {
    let mut m = match pkg_load(o) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    if o.verbose {
        pkg::plan(&m);
    }
    if let Err(e) = pkg::install(&mut m, o.offline, o.verbose) {
        eprintln!("heddle: {}", e);
        return 1;
    }
    let nt = m.tools.len();
    let nd = m.deps.len();
    println!(
        "heddle: installed {} toolchain package{} and {} dependenc{}",
        nt,
        if nt == 1 { "" } else { "s" },
        nd,
        if nd == 1 { "y" } else { "ies" }
    );
    if nt != 0 || nd != 0 {
        println!("heddle: store {}\nheddle: lock  {}", m.store, m.lock_path);
    }
    0
}

pub fn heddle_env_verify(o: &HeddleOpts) -> i32 {
    let mut m = match pkg_load(o) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    if let Err(e) = pkg::verify(&mut m, o.verbose) {
        eprintln!("heddle: {}", e);
        return 1;
    }
    println!(
        "heddle: environment matches {} ({} toolchain, {} dependenc{})",
        m.lock_path,
        m.tools.len(),
        m.deps.len(),
        if m.deps.len() == 1 { "y" } else { "ies" }
    );
    0
}

fn resolve_prefix(o: &HeddleOpts, p: &Project) -> Result<String, String> {
    if let Some(pf) = &o.prefix {
        if !pf.is_empty() {
            return Ok(pf.clone());
        }
    }
    let cfg = format!("{}/heddle.toml", p.root);
    if let Ok(t) = Toml::parse(&cfg) {
        if let Some(pf) = t.str_of("install", "prefix") {
            if !pf.is_empty() {
                return Ok(pf.to_string());
            }
        }
    }
    Err("no install prefix; pass --prefix DIR or set [install] prefix".to_string())
}

pub fn heddle_install(o: &HeddleOpts) -> i32 {
    let mut p = match load(o) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    let rules = match install::load(&p) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    if !install::has_any(&rules) {
        eprintln!("heddle: no [target.*.install] rules to install");
        return 1;
    }
    let prefix = match resolve_prefix(o, &p) {
        Ok(x) => x,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    if let Some(t) = &o.target {
        if project::target(&p, t).is_none() {
            eprintln!("heddle: unknown target '{}'", t);
            return 1;
        }
    }
    let pl = match install::plan(
        &rules,
        &mut p,
        o.target.as_deref(),
        &prefix,
        o.destdir.as_deref(),
    ) {
        Ok(x) => x,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    if o.dry_run {
        install::dry_run(&pl);
        return 0;
    }
    let log = install::log_path(&p.root, o.destdir.as_deref(), &prefix);
    if let Err(e) = install::run(&pl, &log, o.verbose) {
        eprintln!("heddle: {}", e);
        return 1;
    }
    let n = pl.items.len();
    println!(
        "heddle: installed {} file{} to {}{}",
        n,
        if n == 1 { "" } else { "s" },
        o.destdir.clone().unwrap_or_default(),
        prefix
    );
    println!("heddle: log {}", log);
    0
}

pub fn heddle_uninstall(o: &HeddleOpts) -> i32 {
    let p = match load(o) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    let prefix = match resolve_prefix(o, &p) {
        Ok(x) => x,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    let log = install::log_path(&p.root, o.destdir.as_deref(), &prefix);
    match install::uninstall(&log, o.verbose) {
        Ok(()) => 0,
        Err(e) => {
            eprintln!("heddle: {}", e);
            1
        }
    }
}

pub fn heddle_compdb(o: &HeddleOpts) -> i32 {
    let p = match load(o) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    let out = o
        .compile_db
        .clone()
        .unwrap_or_else(|| "compile_commands.json".to_string());
    if let Err(e) = emit::compile_db(&p, o.target.as_deref(), &out) {
        eprintln!("heddle: {}", e);
        return 1;
    }
    println!("heddle: wrote {}", out);
    0
}
