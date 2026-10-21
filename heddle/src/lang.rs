use crate::toml::Toml;
use std::sync::{Mutex, OnceLock};

#[derive(Clone, Default)]
pub struct Lang {
    pub exts: Vec<String>,
    pub cmd: String,
    pub args: Vec<String>,
    pub outext: Option<String>,
    pub cflags: bool,
    pub fmt: bool,
}

fn registry() -> &'static Mutex<Vec<Lang>> {
    static R: OnceLock<Mutex<Vec<Lang>>> = OnceLock::new();
    R.get_or_init(|| Mutex::new(Vec::new()))
}

fn ext_match(path: &str, ext: &str) -> bool {
    let n = path.len();
    let m = ext.len();
    if m == 0 || n < m + 1 {
        return false;
    }
    let b = path.as_bytes();
    if b[n - m - 1] != b'.' {
        return false;
    }
    &path[n - m..] == ext
}

pub fn add(ext: &str, cmd: &str, args: &[String], outext: Option<&str>, cflags: bool, fmt: bool) {
    let mut r = registry().lock().unwrap();
    if r.len() >= 32 {
        return;
    }
    r.push(Lang {
        exts: vec![ext.to_string()],
        cmd: cmd.to_string(),
        args: args.to_vec(),
        outext: outext.map(|s| s.to_string()),
        cflags,
        fmt,
    });
}

pub fn init_builtin() {
    static DONE: Mutex<bool> = Mutex::new(false);
    {
        let mut d = DONE.lock().unwrap();
        if *d {
            return;
        }
        *d = true;
    }
    let cc = vec!["-c".to_string()];
    add("c", "cc", &cc, None, true, false);
    add("cc", "c++", &cc, None, true, false);
    add("cpp", "c++", &cc, None, true, false);
    add("cxx", "c++", &cc, None, true, false);
    add("c++", "c++", &cc, None, true, false);
    add("S", "cc", &cc, None, true, false);
    add("s", "cc", &cc, None, true, false);
    add("asm", "nasm", &[], None, false, true);
}

pub fn for_path(path: &str) -> Option<Lang> {
    let r = registry().lock().unwrap();
    for l in r.iter() {
        for e in &l.exts {
            if ext_match(path, e) {
                return Some(l.clone());
            }
        }
    }
    None
}

pub fn load_toml(t: &Toml) {
    for name in t.sections("lang.") {
        let sect = format!("lang.{}", name);
        let cmd = match t.str_of(&sect, "cmd") {
            Some(c) => c.to_string(),
            None => continue,
        };
        let ext = t
            .str_of(&sect, "ext")
            .map(|s| s.to_string())
            .unwrap_or_else(|| name.clone());
        let out = t.str_of(&sect, "out").map(|s| s.to_string());
        let mut args: Vec<String> = Vec::new();
        for k in 0..16usize {
            match t.arr(&sect, "args", k) {
                Some(a) => args.push(a.to_string()),
                None => break,
            }
        }
        add(
            &ext,
            &cmd,
            &args,
            out.as_deref(),
            t.bool_of(&sect, "cflags", false),
            t.bool_of(&sect, "fmt", false),
        );
    }
}
