use std::io::Write;

use crate::emit;
use crate::heddle::heddle_run;
use crate::model::{HeddleOpts, Project};
use crate::project;
use crate::sys;
use crate::toml::{parse_atoi, Toml};
use loom_core::proc;

#[cfg(windows)]
const TEST_OUT: &str = ".heddle\\test.out";
#[cfg(not(windows))]
const TEST_OUT: &str = ".heddle/test.out";

struct TestCase {
    name: String,
    args: Vec<String>,
    expect: i32,
    stdout_match: Option<String>,
}

fn load(o: &HeddleOpts) -> Result<Project, String> {
    if !sys::chdir(&o.root) {
        return Err(format!("cannot enter project directory '{}'", o.root));
    }
    project::set_target(o.target.as_deref());
    let p = project::load(".", o.toolchain.as_deref())?;
    emit::project_check(&p)?;
    Ok(p)
}

fn test_load(p: &Project) -> Result<Vec<TestCase>, String> {
    let cfg = format!("{}/heddle.toml", p.root);
    let t = Toml::parse(&cfg)?;
    let mut out: Vec<TestCase> = Vec::new();
    for tg in &p.targets {
        let sect = format!("target.{}.test", tg.name);
        let stdo = t.str_of(&sect, "stdout");
        let code = t.str_of(&sect, "expect");
        let has = stdo.is_some() || code.is_some() || t.arr(&sect, "args", 0).is_some();
        if !has {
            continue;
        }
        let expect = match code {
            Some(c) => parse_atoi(c) as i32,
            None => 0,
        };
        let mut c = TestCase {
            name: tg.name.clone(),
            args: Vec::new(),
            expect,
            stdout_match: stdo.map(|s| s.to_string()),
        };
        let mut k = 0usize;
        while let Some(a) = t.arr(&sect, "args", k) {
            c.args.push(a.to_string());
            k += 1;
        }
        out.push(c);
    }
    Ok(out)
}

#[cfg(windows)]
fn resolve_bin(bin: &mut String) {
    if sys::stat(bin).is_some() {
        return;
    }
    let l = bin.len();
    if l < 4 || !bin.ends_with(".exe") {
        if l + 4 < 4096 {
            bin.push_str(".exe");
        }
    }
}

#[cfg(not(windows))]
fn resolve_bin(_bin: &mut String) {}

fn run_case(c: &TestCase, bin: &str, verbose: bool, out: &mut String) -> i32 {
    let mut argv: Vec<String> = vec![bin.to_string()];
    argv.extend(c.args.iter().cloned());

    if verbose {
        print!("heddle:");
        for a in &argv {
            print!(" {}", a);
        }
        println!();
    }

    let r = match proc::run(&argv, None, TEST_OUT, &[]) {
        Some(r) => r,
        None => return -1,
    };

    out.clear();
    if let Ok(data) = std::fs::read(TEST_OUT) {
        let take = data.len().min(8191);
        *out = String::from_utf8_lossy(&data[..take]).into_owned();
    }

    if r.signaled {
        -1
    } else {
        r.exit_code
    }
}

pub fn heddle_test(o: &HeddleOpts) -> i32 {
    let p = match load(o) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };

    let set = match test_load(&p) {
        Ok(s) => s,
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

    if set.is_empty() {
        eprintln!("heddle: no tests declared (add [target.NAME.test] to heddle.toml)");
        return 1;
    }

    let mut pass = 0i32;
    let mut fail = 0i32;

    for c in &set {
        if let Some(t) = &o.target {
            if t != &c.name {
                continue;
            }
        }

        if project::target(&p, &c.name).is_none() {
            eprintln!("heddle: unknown target '{}'", c.name);
            fail += 1;
            continue;
        }

        let mut bo = o.clone();
        bo.target = Some(c.name.clone());

        print!("[{}] build {} ... ", c.name, c.name);
        let _ = std::io::stdout().flush();

        if heddle_run(&bo) != 0 {
            println!("FAIL (build)");
            fail += 1;
            continue;
        }

        let art = match project::target(&p, &c.name).and_then(|t| emit::artifact(&p, t)) {
            Some(b) => b,
            None => {
                println!("FAIL (no artifact)");
                fail += 1;
                continue;
            }
        };

        let mut norm = art;
        resolve_bin(&mut norm);

        let mut stdout_buf = String::new();
        let rc = run_case(c, &norm, o.verbose, &mut stdout_buf);

        let mut ok = rc == c.expect;
        if ok {
            if let Some(m) = &c.stdout_match {
                if !stdout_buf.contains(m.as_str()) {
                    ok = false;
                }
            }
        }

        if ok {
            println!("ok (exit {})", rc);
            pass += 1;
        } else {
            println!("FAIL (exit {}, want {})", rc, c.expect);
            if let Some(m) = &c.stdout_match {
                if !stdout_buf.contains(m.as_str()) {
                    println!("      stdout does not contain '{}'", m);
                    if !stdout_buf.is_empty() {
                        println!("      got: {}", stdout_buf);
                    }
                }
            }
            fail += 1;
        }
    }

    println!("heddle: {} passed, {} failed", pass, fail);

    if fail != 0 {
        1
    } else {
        0
    }
}
