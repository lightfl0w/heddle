mod emit;
mod fmt;
mod glob;
mod heddle;
mod init;
mod install;
mod json;
mod lang;
mod ldconv;
mod link;
mod migrate;
mod model;
mod pkg;
mod project;
mod recipe;
mod star;
mod sys;
mod test;
mod toml;
mod toolchain;
mod vcpkg;

use loom_core::version::version_string;
use model::HeddleOpts;

#[derive(Default)]
struct Args {
    o: HeddleOpts,
    check_only: bool,
    list_tools: bool,
    exec_after: bool,
    tool_install: bool,
    tool_plan: bool,
    env_verify: bool,
    do_test: bool,
    do_watch: bool,
    do_install: bool,
    do_uninstall: bool,
    do_fmt: bool,
    dashdash: bool,
}

fn usage(prog: &str) -> i32 {
    eprintln!(
        "usage: {p} [build] TARGET
       {p} run TARGET
       {p} test [TARGET]        build and run declared tests
       {p} watch [TARGET]       rebuild whenever sources change
       {p} fmt [TARGET]        format sources with clang-format
       {p} init [TYPE] [DIR]   create a new project
       {p} check
       {p} toolchains
       {p} tool install        restore toolchain + dependencies
       {p} tool plan           print the resolved package plan
       {p} env verify          verify the environment matches the lock
       {p} install [TARGET]    install build products
       {p} uninstall           remove what install wrote
       {p} vcpkg <cmd>         vcpkg package compatibility
       {p} migrate [DIR]       convert CMakeLists.txt / xmake.lua

options:
  -C DIR            project root (default .)
  -j N              parallel jobs
  -t NAME           toolchain, default auto-detect
  -v, --verbose     print toolchain and cache stats
  --cache DIR       CAS root
  --remote URL      remote CAS, dir or http(s)://
  --no-cache        disable cache
  --registry DIR|URL  package registry (default $HEDDLE_REGISTRY)
  --offline         never contact the registry
  --prefix DIR      install prefix (required for install)
  --destdir DIR     stage under DIR, do not touch the system
  --dry-run         print the install plan only
  --compile-db[=FILE]  write compile_commands.json and exit
  --interval SEC    watch poll interval (default 0.5)
",
        p = prog
    );
    2
}

fn take_opt(arg: &str, name: &str, args: &[String], i: &mut usize) -> Result<Option<String>, i32> {
    if arg == name {
        if *i + 1 >= args.len() {
            eprintln!("heddle: {} needs a value", name);
            return Err(2);
        }
        *i += 1;
        return Ok(Some(args[*i].clone()));
    }
    let nb = name.as_bytes();
    if nb.len() >= 2 && nb[0] == b'-' && nb[1] == b'-' && arg.starts_with(name) {
        let rest = &arg[name.len()..];
        if let Some(v) = rest.strip_prefix('=') {
            if !v.is_empty() {
                return Ok(Some(v.to_string()));
            }
        }
    }
    if nb.get(1) != Some(&b'-') && arg.starts_with(name) && arg.len() > name.len() {
        return Ok(Some(arg[name.len()..].to_string()));
    }
    Ok(None)
}

fn take_value_opts(arg: &str, args: &[String], i: &mut usize, a: &mut Args) -> Result<bool, i32> {
    if let Some(v) = take_opt(arg, "-C", args, i)? {
        a.o.root = v;
        return Ok(true);
    }
    if let Some(v) = take_opt(arg, "-j", args, i)? {
        a.o.jobs = v.parse().map_err(|_| {
            eprintln!("heddle: invalid job count '{}'", v);
            2
        })?;
        return Ok(true);
    }
    if let Some(v) = take_opt(arg, "-t", args, i)? {
        a.o.toolchain = Some(v);
        return Ok(true);
    }
    if let Some(v) = take_opt(arg, "--cache", args, i)? {
        a.o.cache = Some(v);
        return Ok(true);
    }
    if let Some(v) = take_opt(arg, "--remote", args, i)? {
        a.o.remote = Some(v);
        return Ok(true);
    }
    if let Some(v) = take_opt(arg, "--registry", args, i)? {
        a.o.registry = Some(v);
        return Ok(true);
    }
    if let Some(v) = take_opt(arg, "--prefix", args, i)? {
        a.o.prefix = Some(v);
        return Ok(true);
    }
    if let Some(v) = take_opt(arg, "--destdir", args, i)? {
        a.o.destdir = Some(v);
        return Ok(true);
    }
    if let Some(v) = take_opt(arg, "--interval", args, i)? {
        let f: f64 = v.parse().unwrap_or(0.0);
        a.o.interval_ms = if f > 0.0 { (f * 1000.0) as i64 } else { 500 };
        return Ok(true);
    }
    Ok(false)
}

fn parse(args: &[String], a: &mut Args) -> i32 {
    a.o.root = ".".to_string();
    a.o.registry = std::env::var("HEDDLE_REGISTRY").ok();
    let mut sub = 0i32;
    let mut i = 1usize;
    while i < args.len() {
        let arg = args[i].clone();
        if sub == 1 {
            sub = 0;
            if arg == "install" {
                a.tool_install = true;
                i += 1;
                continue;
            }
            if arg == "plan" || arg == "list" {
                a.tool_plan = true;
                i += 1;
                continue;
            }
            eprintln!("heddle: unknown tool subcommand '{}'", arg);
            return 2;
        }
        if sub == 2 {
            sub = 0;
            if arg == "verify" {
                a.env_verify = true;
                i += 1;
                continue;
            }
            eprintln!("heddle: unknown env subcommand '{}'", arg);
            return 2;
        }

        if arg == "--version" || arg == "-V" {
            println!("heddle {}", version_string());
            std::process::exit(0);
        }

        let mut consumed = false;
        if arg == "--no-cache" {
            a.o.no_cache = true;
            consumed = true;
        } else if arg.starts_with("--compile-db") {
            let b = arg.as_bytes();
            if b.len() == 12 {
                a.o.compile_db = Some("compile_commands.json".to_string());
            } else if b[12] == b'=' {
                a.o.compile_db = Some(arg[13..].to_string());
            }
            consumed = true;
        } else if arg == "--offline" {
            a.o.offline = true;
            consumed = true;
        } else if arg == "-v" || arg == "--verbose" {
            a.o.verbose = true;
            consumed = true;
        } else if arg == "-h" || arg == "--help" {
            return usage(args.first().map(|s| s.as_str()).unwrap_or("heddle"));
        } else if arg == "toolchains" {
            a.list_tools = true;
            consumed = true;
        } else if arg == "check" {
            a.check_only = true;
            consumed = true;
        } else if arg == "install" {
            a.do_install = true;
            consumed = true;
        } else if arg == "uninstall" {
            a.do_uninstall = true;
            consumed = true;
        } else if arg == "--dry-run" {
            a.o.dry_run = true;
            consumed = true;
        } else if arg == "tool" {
            sub = 1;
            consumed = true;
        } else if arg == "env" {
            sub = 2;
            consumed = true;
        } else if arg == "build" {
            consumed = true;
        } else if arg == "run" {
            a.exec_after = true;
            consumed = true;
        } else if arg == "--" {
            if !a.exec_after {
                eprintln!("heddle: unknown option '--'");
                return 2;
            }
            a.dashdash = true;
            consumed = true;
        } else if arg == "test" {
            a.do_test = true;
            consumed = true;
        } else if arg == "watch" {
            a.do_watch = true;
            consumed = true;
        } else if arg == "fmt" {
            a.do_fmt = true;
            consumed = true;
        }

        if !consumed {
            match take_value_opts(&arg, args, &mut i, a) {
                Ok(true) => {
                    i += 1;
                    continue;
                }
                Ok(false) => {}
                Err(c) => return c,
            }
        }

        if !consumed {
            if arg.starts_with('-') {
                if a.exec_after && a.o.target.is_some() && a.dashdash {
                    a.o.run_args.push(arg.clone());
                    i += 1;
                    continue;
                }
                if a.exec_after && a.o.target.is_some() {
                    eprintln!("heddle: unknown option '{}'", arg);
                    eprintln!(
                        "heddle: pass program arguments after '--', e.g. heddle run TARGET -- {}",
                        arg
                    );
                    return 2;
                }
                eprintln!("heddle: unknown option '{}'", arg);
                return 2;
            } else if a.exec_after && a.o.target.is_some() {
                a.o.run_args.push(arg.clone());
            } else if a.o.target.is_none() {
                a.o.target = Some(arg.clone());
            } else {
                eprintln!("heddle: unexpected '{}'", arg);
                return 2;
            }
        }

        i += 1;
    }

    if sub != 0 {
        eprintln!("heddle: incomplete command");
        return 2;
    }

    if a.list_tools
        || a.check_only
        || a.tool_install
        || a.tool_plan
        || a.env_verify
        || a.do_install
        || a.do_uninstall
        || a.do_test
        || a.do_fmt
        || a.o.compile_db.is_some()
    {
        return 0;
    }

    if a.o.target.is_none() {
        return usage(args.first().map(|s| s.as_str()).unwrap_or("heddle"));
    }

    0
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() >= 3 && args[1] == "ldconv" {
        std::process::exit(ldconv::heddle_ldconv(&args));
    }
    if args.len() >= 2 && args[1] == "init" {
        std::process::exit(init::heddle_init(&args));
    }
    if args.len() >= 2 && args[1] == "vcpkg" {
        std::process::exit(vcpkg::heddle_vcpkg(&args));
    }
    if args.len() >= 2 && args[1] == "migrate" {
        std::process::exit(migrate::heddle_migrate(&args));
    }

    emit::set_self(args.first().map(|s| s.as_str()).unwrap_or("heddle"));

    let mut a = Args::default();
    let rc = parse(&args, &mut a);
    if rc != 0 {
        std::process::exit(rc);
    }
    if a.list_tools {
        std::process::exit(heddle::heddle_toolchains());
    }
    if a.check_only {
        std::process::exit(heddle::heddle_check(&a.o));
    }
    if a.o.compile_db.is_some() {
        std::process::exit(heddle::heddle_compdb(&a.o));
    }
    if a.do_install {
        std::process::exit(heddle::heddle_install(&a.o));
    }
    if a.do_uninstall {
        std::process::exit(heddle::heddle_uninstall(&a.o));
    }
    if a.tool_install {
        std::process::exit(heddle::heddle_tool_install(&a.o));
    }
    if a.tool_plan {
        std::process::exit(heddle::heddle_tool_plan(&a.o));
    }
    if a.env_verify {
        std::process::exit(heddle::heddle_env_verify(&a.o));
    }
    if a.do_test {
        std::process::exit(test::heddle_test(&a.o));
    }
    if a.do_watch {
        std::process::exit(heddle::heddle_watch(&a.o));
    }
    if a.do_fmt {
        std::process::exit(fmt::heddle_fmt(&a.o));
    }
    if a.exec_after {
        std::process::exit(heddle::heddle_exec(&a.o));
    }
    std::process::exit(heddle::heddle_run(&a.o));
}
