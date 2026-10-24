use crate::hash::{hash_read_file, HashDb};
use crate::sys;
use std::collections::HashMap;
use std::process::{Command, Stdio};

pub struct Cas {
    pub root: String,
    pub remote: Option<String>,
    remote_http: bool,
}

fn obj_rel(h: u64) -> String {
    format!("objects/{:02x}/{:014x}", h & 0xff, h >> 8)
}

fn copy_file(from: &str, to: &str) -> bool {
    if let Some(parent) = std::path::Path::new(to).parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    std::fs::copy(from, to).is_ok()
}

fn curl_ok(args: &[&str]) -> bool {
    let mut c = Command::new("curl");
    c.args(args);
    c.stderr(Stdio::null());
    matches!(c.status(), Ok(s) if s.success())
}

impl Cas {
    pub fn open(root: &str, remote: Option<&str>) -> Cas {
        sys::mkpath(root);
        Cas {
            root: root.to_string(),
            remote: remote.map(|s| s.to_string()),
            remote_http: remote
                .map(|r| r.starts_with("http://") || r.starts_with("https://"))
                .unwrap_or(false),
        }
    }

    fn obj_path(&self, h: u64) -> String {
        format!("{}/{}", self.root, obj_rel(h))
    }

    fn remote_fetch(&self, rel: &str, dest: &str) -> bool {
        let remote = match &self.remote {
            Some(r) => r,
            None => return false,
        };
        if self.remote_http {
            let url = format!("{}/{}", remote, rel);
            return curl_ok(&["-fsS", "-o", dest, &url]);
        }
        copy_file(&format!("{}/{}", remote, rel), dest)
    }

    fn remote_push(&self, rel: &str, src: &str) -> bool {
        let remote = match &self.remote {
            Some(r) => r,
            None => return false,
        };
        if self.remote_http {
            let url = format!("{}/{}", remote, rel);
            return curl_ok(&["-fsS", "-T", src, &url]);
        }
        let dst = format!("{}/{}", remote, rel);
        if sys::stat(&dst).is_some() {
            return true;
        }
        copy_file(src, &dst)
    }

    pub fn has(&self, h: u64) -> bool {
        let rel = obj_rel(h);
        let path = self.obj_path(h);
        if sys::stat(&path).is_some() {
            return true;
        }
        if self.remote_http {
            let url = format!("{}/{}", self.remote.as_deref().unwrap_or(""), rel);
            if !curl_ok(&["-fsI", &url]) {
                return false;
            }
        }
        self.remote_fetch(&rel, &path)
    }

    pub fn put_file(&self, path: &str) -> u64 {
        let h = hash_read_file(None, path);
        if h == 0 {
            return 0;
        }
        self.put_hashed(path, h);
        h
    }

    pub fn put_hashed(&self, path: &str, h: u64) -> bool {
        if h == 0 {
            return false;
        }
        let dst = self.obj_path(h);
        if sys::stat(&dst).is_some() {
            return true;
        }
        if !copy_file(path, &dst) {
            return false;
        }
        self.remote_push(&obj_rel(h), &dst);
        true
    }

    pub fn get_file(&self, h: u64, path: &str) -> bool {
        let src = self.obj_path(h);
        if sys::stat(&src).is_none() && !self.has(h) {
            return false;
        }
        copy_file(&src, path)
    }

    pub fn put_bytes(&self, h: u64, p: &[u8]) -> bool {
        let dst = self.obj_path(h);
        if sys::stat(&dst).is_some() {
            return true;
        }
        if !sys::write_bytes(&dst, p) {
            return false;
        }
        self.remote_push(&obj_rel(h), &dst);
        true
    }

    pub fn get_bytes(&self, h: u64) -> Option<Vec<u8>> {
        let src = self.obj_path(h);
        if sys::stat(&src).is_none() && !self.has(h) {
            return None;
        }
        sys::read_bytes(&src)
    }
}

pub struct AcOut {
    pub hash: u64,
    pub mode: u32,
}

pub struct AcEntry {
    pub key: u64,
    pub outs: Vec<AcOut>,
}

pub struct ActionCache {
    pub items: Vec<AcEntry>,
    pub root: String,
    index: HashMap<u64, usize>,
}

impl ActionCache {
    pub fn new(root: &str) -> ActionCache {
        ActionCache {
            items: Vec::new(),
            root: root.to_string(),
            index: HashMap::new(),
        }
    }

    pub fn find(&self, key: u64) -> Option<usize> {
        self.index.get(&key).copied()
    }

    pub fn put(&mut self, key: u64, outs: &[AcOut]) {
        match self.find(key) {
            Some(i) => {
                self.items[i].outs = outs
                    .iter()
                    .map(|o| AcOut {
                        hash: o.hash,
                        mode: o.mode,
                    })
                    .collect();
            }
            None => {
                self.items.push(AcEntry {
                    key,
                    outs: outs
                        .iter()
                        .map(|o| AcOut {
                            hash: o.hash,
                            mode: o.mode,
                        })
                        .collect(),
                });
                self.index.insert(key, self.items.len() - 1);
            }
        }
    }

    pub fn load(&mut self) -> bool {
        let path = format!("{}/actions", self.root);
        let text = match sys::read_to_string(&path) {
            Some(t) => t,
            None => return false,
        };
        for line in text.lines() {
            let mut it = line.split_whitespace();
            if it.next() != Some("A") {
                continue;
            }
            let key_s = match it.next() {
                Some(v) => v,
                None => continue,
            };
            let n_s = match it.next() {
                Some(v) => v,
                None => continue,
            };
            let key = u64::from_str_radix(key_s, 16).unwrap_or(0);
            let count: i64 = n_s.parse().unwrap_or(0);
            let mut outs: Vec<AcOut> = Vec::new();
            let mut ok = true;
            for _ in 0..count {
                let tok = match it.next() {
                    Some(v) => v,
                    None => {
                        ok = false;
                        break;
                    }
                };
                let (h, m) = match tok.split_once(':') {
                    Some((a, b)) => (a, Some(b)),
                    None => (tok, None),
                };
                outs.push(AcOut {
                    hash: u64::from_str_radix(h, 16).unwrap_or(0),
                    mode: m.and_then(|x| u32::from_str_radix(x, 8).ok()).unwrap_or(0o644),
                });
            }
            if ok && outs.len() as i64 == count {
                self.put(key, &outs);
            }
        }
        true
    }

    pub fn save(&self) -> bool {
        let path = format!("{}/actions", self.root);
        let mut out = String::new();
        for e in &self.items {
            out.push_str(&format!("A {:016x} {}", e.key, e.outs.len()));
            for o in &e.outs {
                out.push_str(&format!(" {:016x}:{:o}", o.hash, o.mode));
            }
            out.push('\n');
        }
        sys::write_bytes(&path, out.as_bytes())
    }
}

#[allow(dead_code)]
pub fn file_hash(db: &mut HashDb, path: &str) -> u64 {
    hash_read_file(Some(db), path)
}
