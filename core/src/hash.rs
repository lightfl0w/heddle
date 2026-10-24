use crate::sys;
use std::collections::HashMap;
use std::io::{BufReader, Read};
use xxhash_rust::xxh64;

pub const HASH_SEED: u64 = 0;
const HASH_CHUNK: usize = 256 * 1024;

pub struct HashEntry {
    pub path: String,
    pub value: u64,
    pub mtime_ns: i64,
    pub size: i64,
}

pub struct HashDb {
    pub items: Vec<HashEntry>,
    index: HashMap<String, usize>,
}

impl Default for HashDb {
    fn default() -> Self {
        Self::new()
    }
}

impl HashDb {
    pub fn new() -> HashDb {
        HashDb {
            items: Vec::new(),
            index: HashMap::new(),
        }
    }

    pub fn find(&self, path: &str) -> Option<usize> {
        self.index.get(path).copied()
    }

    fn insert(&mut self, path: &str) -> usize {
        if let Some(i) = self.find(path) {
            return i;
        }
        let i = self.items.len();
        self.items.push(HashEntry {
            path: path.to_string(),
            value: 0,
            mtime_ns: 0,
            size: 0,
        });
        self.index.insert(path.to_string(), i);
        i
    }

    pub fn load(&mut self, path: &str) -> bool {
        let text = match sys::read_to_string(path) {
            Some(t) => t,
            None => return false,
        };
        for line in text.lines() {
            let mut t = line.splitn(5, char::is_whitespace);
            let tag = match t.next() {
                Some(x) => x,
                None => continue,
            };
            if tag != "H" {
                continue;
            }
            let hex = t.next();
            let mtime = t.next();
            let size = t.next();
            let file = t.next();
            let (hex, mtime, size, file) = match (hex, mtime, size, file) {
                (Some(a), Some(b), Some(c), Some(d)) => (a, b, c, d),
                _ => continue,
            };
            let i = self.insert(file);
            self.items[i].value = u64::from_str_radix(hex, 16).unwrap_or(0);
            self.items[i].mtime_ns = mtime.parse().unwrap_or(0);
            self.items[i].size = size.parse().unwrap_or(0);
        }
        true
    }

    pub fn save(&self, path: &str) -> bool {
        let mut out = String::new();
        for e in &self.items {
            out.push_str(&format!(
                "H {:016x} {} {} {}\n",
                e.value, e.mtime_ns, e.size, e.path
            ));
        }
        sys::write_bytes_if_changed(path, out.as_bytes())
    }
}

pub fn hash_bytes(seed: u64, p: &[u8]) -> u64 {
    xxh64::xxh64(p, seed)
}

pub fn hash_text(seed: u64, s: &str) -> u64 {
    let mut h = xxh64::Xxh64::new(seed);
    h.update(s.as_bytes());
    h.update(&[0u8]);
    h.digest()
}

pub fn hash_u64(seed: u64, v: u64) -> u64 {
    hash_bytes(seed, &v.to_le_bytes())
}

pub fn hash_read_file(mut db: Option<&mut HashDb>, path: &str) -> u64 {
    let st = match sys::stat(path) {
        Some(s) => s,
        None => return 0,
    };
    let cached = db.as_ref().and_then(|d| d.find(path));
    if let Some(i) = cached {
        let d = db.as_ref().unwrap();
        let e = &d.items[i];
        if e.value != 0 && e.mtime_ns == st.mtime_ns && e.size == st.size {
            return e.value;
        }
    }
    let file = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(_) => return 0,
    };
    let mut h = xxh64::Xxh64::new(HASH_SEED);
    let mut reader = BufReader::with_capacity(HASH_CHUNK, file);
    let mut buf = vec![0u8; HASH_CHUNK];
    loop {
        match reader.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => h.update(&buf[..n]),
            Err(_) => return 0,
        }
    }
    let h = h.digest();
    if let Some(d) = db.as_mut() {
        let i = match cached {
            Some(i) => i,
            None => d.insert(path),
        };
        d.items[i].value = h;
        d.items[i].mtime_ns = st.mtime_ns;
        d.items[i].size = st.size;
    }
    h
}
