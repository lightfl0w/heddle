use crate::sys;

#[derive(Default, Clone)]
pub struct TomlTable {
    pub name: String,
    pub items: Vec<(String, String)>,
}

#[derive(Default, Clone)]
pub struct Toml {
    pub tables: Vec<TomlTable>,
}

fn trim(s: &str) -> &str {
    s.trim_matches(|c: char| c.is_whitespace())
}

fn table_find(t: &Toml, name: &str) -> Option<usize> {
    t.tables.iter().position(|x| x.name == name)
}

fn table_add(t: &mut Toml, name: &str) -> usize {
    if let Some(i) = table_find(t, name) {
        return i;
    }
    t.tables.push(TomlTable {
        name: name.to_string(),
        items: Vec::new(),
    });
    t.tables.len() - 1
}

fn table_put(tab: &mut TomlTable, key: &str, val: &str) {
    if let Some(e) = tab.items.iter_mut().find(|(k, _)| k == key) {
        e.1 = val.to_string();
        return;
    }
    tab.items.push((key.to_string(), val.to_string()));
}

pub fn unquote(s: &str) -> String {
    let b = s.as_bytes();
    let mut start = 0usize;
    let mut n = b.len();
    while n > 0 && (b[n - 1] as char).is_whitespace() {
        n -= 1;
    }
    while n > 0 && (b[start] as char).is_whitespace() {
        start += 1;
        n -= 1;
    }
    if n >= 2 && b[start] == b'"' && b[start + n - 1] == b'"' {
        start += 1;
        n -= 2;
    }
    let seg = &s[start..start + n];
    let mut out = String::new();
    let mut it = seg.chars();
    while let Some(c) = it.next() {
        if c == '\\' {
            match it.next() {
                Some('n') => out.push('\n'),
                Some('t') => out.push('\t'),
                Some('r') => out.push('\r'),
                Some('\\') => out.push('\\'),
                Some('"') => out.push('"'),
                Some(e) => out.push(e),
                None => out.push('\\'),
            }
            continue;
        }
        out.push(c);
    }
    out
}

impl Toml {
    pub fn new() -> Toml {
        let mut t = Toml { tables: Vec::new() };
        table_add(&mut t, "");
        t
    }

    pub fn parse(path: &str) -> Result<Toml, String> {
        let text = match sys::read_to_string(path) {
            Some(t) => t,
            None => return Err(format!("cannot read {}", path)),
        };
        let mut t = Toml::new();
        let mut cur = 0usize;
        let mut lineno = 0usize;
        for raw in text.split('\n') {
            lineno += 1;
            let s = trim(raw);
            if s.is_empty() || s.starts_with('#') {
                continue;
            }
            if s.starts_with('[') {
                let close = match s.rfind(']') {
                    Some(i) => i,
                    None => return Err(format!("line {}: missing ']'", lineno)),
                };
                cur = table_add(&mut t, trim(&s[1..close]));
                continue;
            }
            let eq = match s.find('=') {
                Some(i) => i,
                None => return Err(format!("line {}: missing '='", lineno)),
            };
            let key = trim(&s[..eq]);
            if key.is_empty() {
                return Err(format!("line {}: empty key", lineno));
            }
            let val = trim(&s[eq + 1..]);
            if val.starts_with('[') {
                let close = match val.rfind(']') {
                    Some(i) => i,
                    None => return Err(format!("line {}: missing ']'", lineno)),
                };
                let inner = &val[1..close];
                let mut p = 0usize;
                let bb = inner.as_bytes();
                let mut idx = 0usize;
                while p < bb.len() {
                    while p < bb.len() && (bb[p] == b' ' || bb[p] == b'\t') {
                        p += 1;
                    }
                    if p < bb.len() && bb[p] == b',' {
                        p += 1;
                        continue;
                    }
                    if p >= bb.len() {
                        break;
                    }
                    let start;
                    
                    let len = if bb[p] == b'"' {
                        start = p;
                        p += 1;
                        while p < bb.len() {
                            if bb[p] == b'\\' && p + 1 < bb.len() {
                                p += 2;
                                continue;
                            }
                            if bb[p] == b'"' {
                                break;
                            }
                            p += 1;
                        }
                        if p < bb.len() {
                            p += 1;
                        }
                        p - start
                    } else {
                        start = p;
                        while p < bb.len() && bb[p] != b',' {
                            p += 1;
                        }
                        p - start
                    };
                    let item = unquote(&inner[start..start + len]);
                    let norm = format!("{}[{}]", key, idx);
                    idx += 1;
                    table_put(&mut t.tables[cur], &norm, &item);
                    while p < bb.len() && bb[p] != b',' {
                        p += 1;
                    }
                    if p < bb.len() && bb[p] == b',' {
                        p += 1;
                    }
                }
                continue;
            }
            let plain = unquote(val);
            table_put(&mut t.tables[cur], key, &plain);
        }
        Ok(t)
    }

    pub fn str_of(&self, section: &str, key: &str) -> Option<&str> {
        let i = table_find(self, section)?;
        self.tables[i]
            .items
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v.as_str())
    }

    pub fn arr(&self, section: &str, key: &str, index: usize) -> Option<&str> {
        let norm = format!("{}[{}]", key, index);
        self.str_of(section, &norm)
    }

    pub fn bool_of(&self, section: &str, key: &str, fb: bool) -> bool {
        match self.str_of(section, key) {
            Some("true") => true,
            Some("false") => false,
            _ => fb,
        }
    }

    pub fn sections(&self, prefix: &str) -> Vec<String> {
        let mut out = Vec::new();
        for t in &self.tables {
            if !t.name.starts_with(prefix) {
                continue;
            }
            let rest = &t.name[prefix.len()..];
            if rest.contains('.') {
                continue;
            }
            out.push(rest.to_string());
        }
        out
    }
}

pub fn parse_atoi(s: &str) -> i64 {
    let t = s.trim_start();
    let b = t.as_bytes();
    let mut i = 0usize;
    let mut sign = 1i64;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        if b[i] == b'-' {
            sign = -1;
        }
        i += 1;
    }
    let mut v: i64 = 0;
    while i < b.len() && b[i].is_ascii_digit() {
        v = v.wrapping_mul(10).wrapping_add((b[i] - b'0') as i64);
        i += 1;
    }
    v * sign
}
