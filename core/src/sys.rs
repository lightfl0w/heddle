use std::path::Path;
use std::time::Duration;

pub struct SysStat {
    pub size: i64,
    pub mtime_ns: i64,
    pub mode: u32,
    pub is_dir: bool,
}

pub fn stat(path: &str) -> Option<SysStat> {
    let md = std::fs::metadata(path).ok()?;
    let mtime_ns = md
        .modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_nanos() as i64)
        .unwrap_or(0);
    let mode = mode_of(&md);
    Some(SysStat {
        size: md.len() as i64,
        mtime_ns,
        mode,
        is_dir: md.is_dir(),
    })
}

#[cfg(unix)]
fn mode_of(md: &std::fs::Metadata) -> u32 {
    use std::os::unix::fs::PermissionsExt;
    md.permissions().mode()
}

#[cfg(not(unix))]
fn mode_of(md: &std::fs::Metadata) -> u32 {
    if md.is_dir() {
        0o755
    } else {
        0o644
    }
}

pub fn is_dir(path: &str) -> bool {
    stat(path).map(|s| s.is_dir).unwrap_or(false)
}

#[cfg(unix)]
pub fn chmod(path: &str, mode: u32) {
    use std::os::unix::fs::PermissionsExt;
    let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode & 0o7777));
}

#[cfg(not(unix))]
pub fn chmod(_path: &str, _mode: u32) {}

pub fn mkpath(path: &str) {
    let _ = std::fs::create_dir_all(path);
}

pub fn chdir(path: &str) -> bool {
    std::env::set_current_dir(path).is_ok()
}

pub fn sleep_ms(ms: u64) {
    std::thread::sleep(Duration::from_millis(ms));
}

pub fn read_to_string(path: &str) -> Option<String> {
    std::fs::read(path).ok().map(|b| String::from_utf8_lossy(&b).into_owned())
}

pub fn read_bytes(path: &str) -> Option<Vec<u8>> {
    std::fs::read(path).ok()
}

pub fn write_bytes(path: &str, data: &[u8]) -> bool {
    if let Some(parent) = Path::new(path).parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    std::fs::write(path, data).is_ok()
}

pub fn join(dir: &str, name: &str) -> String {
    if dir.is_empty() || dir == "." {
        name.to_string()
    } else if dir.ends_with('/') {
        format!("{}{}", dir, name)
    } else {
        format!("{}/{}", dir, name)
    }
}

pub fn dir_of(path: &str) -> String {
    match path.rfind('/') {
        Some(0) => "/".to_string(),
        Some(i) => path[..i].to_string(),
        None => ".".to_string(),
    }
}

pub fn base_name(path: &str) -> &str {
    match path.rfind('/') {
        Some(i) => &path[i + 1..],
        None => path,
    }
}
