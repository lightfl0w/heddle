use std::path::PathBuf;
use std::process::{Command, Stdio};

#[derive(Default, Clone, Copy)]
pub struct ProcResult {
    pub exit_code: i32,
    pub signaled: bool,
    pub signal: i32,
}

fn from_status(st: std::process::ExitStatus) -> ProcResult {
    #[cfg(unix)]
    {
        use std::os::unix::process::ExitStatusExt;
        if let Some(sig) = st.signal() {
            return ProcResult {
                exit_code: -1,
                signaled: true,
                signal: sig,
            };
        }
    }
    ProcResult {
        exit_code: st.code().unwrap_or(-1),
        signaled: false,
        signal: 0,
    }
}

fn spawn<'a>(
    cmd: &mut Command,
    cwd: Option<&str>,
    env: &[(String, String)],
) -> Option<std::process::ExitStatus> {
    if let Some(d) = cwd {
        cmd.current_dir(d);
    }
    if !env.is_empty() {
        cmd.envs(env.iter().map(|(k, v)| (k, v)));
    }
    cmd.status().ok()
}

pub fn run(
    argv: &[String],
    cwd: Option<&str>,
    log_path: &str,
    env: &[(String, String)],
) -> Option<ProcResult> {
    if argv.is_empty() {
        return None;
    }
    let log = match std::fs::File::create(log_path) {
        Ok(f) => f,
        Err(_) => return None,
    };
    let log2 = log.try_clone().ok()?;
    let mut cmd = Command::new(&argv[0]);
    cmd.args(&argv[1..]);
    cmd.stdin(Stdio::null());
    cmd.stdout(Stdio::from(log));
    cmd.stderr(Stdio::from(log2));
    let st = spawn(&mut cmd, cwd, env)?;
    Some(from_status(st))
}

pub fn run_inherit(
    argv: &[String],
    cwd: Option<&str>,
    env: &[(String, String)],
) -> Option<ProcResult> {
    if argv.is_empty() {
        return None;
    }
    let mut cmd = Command::new(&argv[0]);
    cmd.args(&argv[1..]);
    let st = spawn(&mut cmd, cwd, env)?;
    Some(from_status(st))
}

pub fn shell(cmd_str: &str, cwd: Option<&str>, log_path: &str) -> Option<ProcResult> {
    let argv: Vec<String> = vec!["sh".to_string(), "-c".to_string(), cmd_str.to_string()];
    run(&argv, cwd, log_path, &[])
}

#[allow(dead_code)]
pub fn which(name: &str) -> Option<PathBuf> {
    let paths = std::env::var_os("PATH")?;
    for dir in std::env::split_paths(&paths) {
        let p = dir.join(name);
        if p.is_file() {
            return Some(p);
        }
    }
    None
}
