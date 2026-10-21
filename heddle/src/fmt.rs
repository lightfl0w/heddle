use std::io::Write;

use crate::glob;
use crate::model::HeddleOpts;
use crate::project;
use crate::sys;
use loom_core::proc;

#[cfg(windows)]
const FMT_LOG: &str = ".heddle\\fmt.log";
#[cfg(not(windows))]
const FMT_LOG: &str = ".heddle/fmt.log";

const CLANG_VER: &str = "20";

fn is_header(path: &str) -> bool {
    let dot = match path.rfind('.') {
        Some(i) => i,
        None => return false,
    };
    let ext = &path[dot..];
    ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".inl"
}

fn fmt_lang(path: &str) -> bool {
    let dot = match path.rfind('.') {
        Some(i) => i,
        None => return false,
    };
    let ext = &path[dot..];
    if ext == ".c" || is_header(path) {
        return true;
    }
    ext == ".cc" || ext == ".cpp" || ext == ".cxx"
}

fn add_file(l: &mut Vec<String>, path: &str) {
    let st = match sys::stat(path) {
        Some(s) => s,
        None => return,
    };
    if st.is_dir {
        return;
    }
    if l.iter().any(|x| x == path) {
        return;
    }
    l.push(path.to_string());
}

fn add_headers_in(l: &mut Vec<String>, dir: &str) {
    let pats = ["*.h", "*.hpp", "*.hh", "*.hxx", "*.inl"];
    for p in pats {
        for hit in glob::glob_dir(dir, p) {
            let full = format!("{}/{}", dir, hit);
            if full.len() >= 4096 {
                continue;
            }
            add_file(l, &full);
        }
    }
}

fn collect_target(t: &crate::model::Target, out: &mut Vec<String>) {
    for s in &t.src {
        if fmt_lang(s) {
            add_file(out, s);
        }
    }
    for inc in &t.inc {
        let st = match sys::stat(inc) {
            Some(s) => s,
            None => continue,
        };
        if st.is_dir {
            add_headers_in(out, inc);
        } else if is_header(inc) {
            add_file(out, inc);
        }
    }
}

fn collect_project(p: &crate::model::Project, out: &mut Vec<String>) {
    for t in &p.targets {
        collect_target(t, out);
    }
}

fn skip_dir(name: &str) -> bool {
    if name.starts_with('.') {
        return true;
    }
    matches!(
        name,
        ".git" | ".heddle" | ".xmake" | ".vscode" | "build" | "out" | "node_modules"
    )
}

fn collect_walk(dir: &str, out: &mut Vec<String>, depth: i32) {
    if depth > 12 {
        return;
    }
    let rd = match std::fs::read_dir(dir) {
        Ok(r) => r,
        Err(_) => return,
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
        let full = format!("{}/{}", dir, name);
        if full.len() >= 4096 {
            continue;
        }
        let st = match sys::stat(&full) {
            Some(s) => s,
            None => continue,
        };
        if st.is_dir {
            if !skip_dir(&name) {
                collect_walk(&full, out, depth + 1);
            }
            continue;
        }
        if fmt_lang(&full) {
            add_file(out, &full);
        }
    }
}

fn has_config(root: &str) -> bool {
    let names = [".clang-format", "_clang-format"];
    for n in names {
        let path = format!("{}/{}", root, n);
        if let Some(st) = sys::stat(&path) {
            if !st.is_dir {
                return true;
            }
        }
    }
    false
}

fn find_clang_format() -> Option<String> {
    let env = std::env::var("PATH").unwrap_or_default();
    let second = format!("clang-format-{}", CLANG_VER);
    let names = ["clang-format", second.as_str()];
    for (i, n) in names.iter().enumerate() {
        let probe = if i == 0 {
            n.to_string()
        } else if cfg!(windows) {
            format!("{}.exe", n)
        } else {
            n.to_string()
        };
        for d in env.split(|c| c == ':' || c == ';') {
            if d.is_empty() {
                continue;
            }
            let path = format!("{}/{}", d, probe);
            if path.len() >= 4096 {
                continue;
            }
            if let Some(st) = sys::stat(&path) {
                if !st.is_dir {
                    return Some(path);
                }
            }
        }
    }
    None
}

pub fn heddle_fmt(o: &HeddleOpts) -> i32 {
    if !sys::chdir(&o.root) {
        eprintln!("heddle: cannot enter project directory '{}'", o.root);
        return 1;
    }

    if !has_config(&o.root) {
        eprintln!("heddle: no .clang-format in {}", o.root);
        return 1;
    }

    let tool = match find_clang_format() {
        Some(t) => t,
        None => {
            eprintln!("heddle: clang-format not found on PATH");
            return 1;
        }
    };

    let mut files: Vec<String> = Vec::new();

    if sys::stat("heddle.star").is_some() || sys::stat("heddle.toml").is_some() {
        let proj = match project::load(".", o.toolchain.as_deref()) {
            Ok(p) => p,
            Err(e) => {
                eprintln!("heddle: {}", e);
                return 1;
            }
        };
        if let Some(t) = &o.target {
            if project::target(&proj, t).is_none() {
                eprintln!("heddle: unknown target '{}'", t);
                return 1;
            }
        }
        match &o.target {
            Some(t) => {
                if let Some(tg) = project::target(&proj, t) {
                    collect_target(tg, &mut files);
                }
            }
            None => collect_project(&proj, &mut files),
        }
    } else {
        collect_walk(".", &mut files, 0);
    }

    if files.is_empty() {
        eprintln!("heddle: no C/C++ sources to format");
        return 1;
    }

    sys::mkpath(".heddle");

    let mut bad = 0i32;
    let mut changed = 0i32;

    for f in &files {
        let before = match sys::stat(f) {
            Some(s) => s,
            None => continue,
        };

        if o.verbose {
            println!("heddle: fmt {}", f);
            let _ = std::io::stdout().flush();
        }

        let argv = vec![tool.clone(), "-i".to_string(), f.clone()];
        let ok = match proc::run(&argv, None, FMT_LOG, &[]) {
            Some(r) => !r.signaled && r.exit_code == 0,
            None => false,
        };
        if !ok {
            eprintln!("heddle: clang-format failed on {}", f);
            bad += 1;
            continue;
        }

        if let Some(after) = sys::stat(f) {
            if after.mtime_ns != before.mtime_ns {
                changed += 1;
                println!("  formatted {}", f);
            }
        }
    }

    let _ = std::fs::remove_file(FMT_LOG);

    println!(
        "heddle: {} file{} checked, {} reformatted",
        files.len(),
        if files.len() == 1 { "" } else { "s" },
        changed
    );

    if bad != 0 {
        1
    } else {
        0
    }
}
