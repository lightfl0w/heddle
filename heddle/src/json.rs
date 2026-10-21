use crate::sys;

#[derive(Default, Clone)]
pub struct Json {
    pub items: Vec<(String, String)>,
}

struct Parser<'a> {
    p: usize,
    end: usize,
    j: &'a mut Json,
    err: Option<String>,
    depth: i32,
}

fn is_ws(c: u8) -> bool {
    c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' || c == 0x0b || c == 0x0c
}

impl<'a> Parser<'a> {
    fn skip_ws(&mut self, b: &[u8]) {
        while self.p < self.end && is_ws(b[self.p]) {
            self.p += 1;
        }
    }

    fn read_string(&mut self, b: &[u8]) -> Option<String> {
        if self.p >= self.end || b[self.p] != b'"' {
            return None;
        }
        self.p += 1;
        let mut out: Vec<u8> = Vec::new();
        while self.p < self.end && b[self.p] != b'"' {
            let mut c = b[self.p];
            self.p += 1;
            if c == b'\\' && self.p < self.end {
                let e = b[self.p];
                self.p += 1;
                c = match e {
                    b'n' => b'\n',
                    b't' => b'\t',
                    b'r' => b'\r',
                    b'b' => 8,
                    b'f' => 12,
                    _ => e,
                };
            }
            out.push(c);
        }
        if self.p < self.end {
            self.p += 1;
        }
        Some(String::from_utf8_lossy(&out).into_owned())
    }

    fn read_primitive(&mut self, b: &[u8]) -> String {
        let start = self.p;
        while self.p < self.end {
            let c = b[self.p];
            if c == b',' || c == b'}' || c == b']' || is_ws(c) {
                break;
            }
            self.p += 1;
        }
        String::from_utf8_lossy(&b[start..self.p]).into_owned()
    }

    fn put(&mut self, key: &str, val: &str) {
        self.j.items.push((key.to_string(), val.to_string()));
    }

    fn parse_value(&mut self, b: &[u8], path: &str) {
        self.skip_ws(b);
        if self.p >= self.end {
            return;
        }
        self.depth += 1;
        if self.depth > 64 {
            if self.err.is_none() {
                self.err = Some(format!("{}: json nested too deep", path));
            }
            self.depth -= 1;
            return;
        }
        if b[self.p] == b'{' {
            self.parse_object(b, path);
        } else if b[self.p] == b'[' {
            self.parse_array(b, path);
        } else if b[self.p] == b'"' {
            if let Some(v) = self.read_string(b) {
                self.put(path, &v);
            }
        } else {
            let v = self.read_primitive(b);
            self.put(path, &v);
        }
        self.depth -= 1;
    }

    fn parse_object(&mut self, b: &[u8], path: &str) {
        self.skip_ws(b);
        if self.p >= self.end || b[self.p] != b'{' {
            return;
        }
        self.p += 1;
        loop {
            self.skip_ws(b);
            if self.p >= self.end {
                return;
            }
            if b[self.p] == b'}' {
                self.p += 1;
                return;
            }
            let key = match self.read_string(b) {
                Some(k) => k,
                None => return,
            };
            self.skip_ws(b);
            if self.p < self.end && b[self.p] == b':' {
                self.p += 1;
            }
            let sub = if path.is_empty() {
                key.clone()
            } else {
                format!("{}.{}", path, key)
            };
            self.parse_value(b, &sub);
            self.skip_ws(b);
            if self.p < self.end && b[self.p] == b',' {
                self.p += 1;
                continue;
            }
            if self.p < self.end && b[self.p] == b'}' {
                self.p += 1;
                return;
            }
        }
    }

    fn parse_array(&mut self, b: &[u8], path: &str) {
        self.skip_ws(b);
        if self.p >= self.end || b[self.p] != b'[' {
            return;
        }
        self.p += 1;
        let mut idx = 0usize;
        loop {
            self.skip_ws(b);
            if self.p >= self.end {
                return;
            }
            if b[self.p] == b']' {
                self.p += 1;
                return;
            }
            let sub = format!("{}[{}]", path, idx);
            idx += 1;
            self.parse_value(b, &sub);
            self.skip_ws(b);
            if self.p < self.end && b[self.p] == b',' {
                self.p += 1;
                continue;
            }
            if self.p < self.end && b[self.p] == b']' {
                self.p += 1;
                return;
            }
        }
    }
}

impl Json {
    pub fn new() -> Json {
        Json { items: Vec::new() }
    }

    pub fn parse(path: &str) -> Result<Json, String> {
        let bytes = match sys::read_bytes(path) {
            Some(b) => b,
            None => return Err(format!("cannot read {}", path)),
        };
        let mut j = Json::new();
        let mut s = Parser {
            p: 0,
            end: bytes.len(),
            j: &mut j,
            err: None,
            depth: 0,
        };
        s.skip_ws(&bytes);
        if s.p >= s.end || bytes[s.p] != b'{' {
            return Err(format!("{}: not a json object", path));
        }
        s.parse_value(&bytes, "");
        s.skip_ws(&bytes);
        if s.p < s.end {
            return Err(format!("{}: trailing data in json", path));
        }
        if let Some(e) = s.err {
            return Err(e);
        }
        Ok(j)
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.items
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v.as_str())
    }
}
