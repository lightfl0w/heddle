use loom_core::build::{BuildEngine, BuildOpts};
use loom_core::version;

fn usage(prog: &str) {
    eprintln!(
        "usage: {} -f FILE [-j N] [--retry N] [--cwd DIR] \
         [--logdir DIR] [--stop] [--cache DIR] [--remote URL] \
         [--no-cache]",
        prog
    );
}

struct Options {
    file: Option<String>,
    cwd: Option<String>,
    logdir: String,
    cache_dir: Option<String>,
    remote: Option<String>,
    jobs: usize,
    retry: usize,
    keep_going: bool,
    no_cache: bool,
}

fn opt_inline<'a>(arg: &'a str, prefix: &str) -> Option<&'a str> {
    if let Some(v) = arg.strip_prefix(prefix) {
        if !v.is_empty() {
            return Some(v);
        }
    }
    None
}

fn parse_options(args: &[String]) -> Option<Options> {
    let mut o = Options {
        file: None,
        cwd: None,
        logdir: ".build".to_string(),
        cache_dir: None,
        remote: None,
        jobs: 1,
        retry: 0,
        keep_going: true,
        no_cache: false,
    };
    let mut i = 1usize;
    while i < args.len() {
        let a = args[i].as_str();
        if a == "--version" || a == "-V" {
            println!("loom {}", version::version_string());
            std::process::exit(0);
        } else if a == "-f" && i + 1 < args.len() {
            i += 1;
            o.file = Some(args[i].clone());
        } else if let Some(v) = opt_inline(a, "-f") {
            o.file = Some(v.to_string());
        } else if a == "-j" && i + 1 < args.len() {
            i += 1;
            o.jobs = args[i].parse().unwrap_or(0);
        } else if let Some(v) = opt_inline(a, "-j") {
            o.jobs = v.parse().unwrap_or(0);
        } else if a == "--retry" && i + 1 < args.len() {
            i += 1;
            o.retry = args[i].parse().unwrap_or(0);
        } else if a == "--cwd" && i + 1 < args.len() {
            i += 1;
            o.cwd = Some(args[i].clone());
        } else if a == "--logdir" && i + 1 < args.len() {
            i += 1;
            o.logdir = args[i].clone();
        } else if a == "--stop" {
            o.keep_going = false;
        } else if a == "--cache" && i + 1 < args.len() {
            i += 1;
            o.cache_dir = Some(args[i].clone());
        } else if a == "--remote" && i + 1 < args.len() {
            i += 1;
            o.remote = Some(args[i].clone());
        } else if a == "--no-cache" {
            o.no_cache = true;
        } else {
            eprintln!("error: unknown argument '{}'", a);
            return None;
        }
        i += 1;
    }
    if o.file.is_some() {
        Some(o)
    } else {
        None
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let o = match parse_options(&args) {
        Some(o) => o,
        None => {
            usage(args.first().map(|s| s.as_str()).unwrap_or("loom"));
            std::process::exit(2);
        }
    };
    let jobs = if o.jobs < 1 { 1 } else { o.jobs };
    let bo = BuildOpts {
        graph_file: o.file.unwrap(),
        cwd: o.cwd,
        logdir: o.logdir,
        jobs,
        retry: o.retry,
        keep_going: o.keep_going,
        cache_dir: o.cache_dir,
        remote: o.remote,
        no_cache: o.no_cache,
        env: Vec::new(),
    };
    let mut e = match BuildEngine::open(bo) {
        Ok(e) => e,
        Err(err) => {
            eprintln!("error: {}", err);
            std::process::exit(1);
        }
    };
    let rc = e.run();
    let ran = e.ran;
    let hit = e.cached;
    if hit > 0 {
        eprintln!("loom: {} cached, {} ran", hit, ran);
    }
    std::process::exit(rc);
}
