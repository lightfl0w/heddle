struct Region {
    name: String,
    origin: String,
    length: String,
}

#[derive(Default)]
struct LdScript {
    entry: Option<String>,
    regions: Vec<Region>,
    places: Vec<String>,
}

struct Scan<'a> {
    b: &'a [u8],
    p: usize,
}

fn is_space(c: u8) -> bool {
    c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' || c == 0x0b || c == 0x0c
}

impl<'a> Scan<'a> {
    fn at_end(&self) -> bool {
        self.p >= self.b.len()
    }

    fn cur(&self) -> u8 {
        self.b[self.p]
    }

    fn skip_ws(&mut self) {
        while self.p < self.b.len() && is_space(self.b[self.p]) {
            self.p += 1;
        }
    }

    fn skip_comment(&mut self) {
        if self.p + 1 < self.b.len() && self.b[self.p] == b'/' && self.b[self.p + 1] == b'*' {
            self.p += 2;
            while self.p + 1 < self.b.len()
                && !(self.b[self.p] == b'*' && self.b[self.p + 1] == b'/')
            {
                self.p += 1;
            }
            if self.p + 1 < self.b.len() {
                self.p += 2;
            }
        }
    }

    fn ident(&mut self, cap: usize) -> Option<String> {
        self.skip_ws();
        let mut out: Vec<u8> = Vec::new();
        while self.p < self.b.len() {
            let c = self.b[self.p];
            if c.is_ascii_alphanumeric() || c == b'_' || c == b'.' || c == b'-' {
                if out.len() + 1 < cap {
                    out.push(c);
                }
                self.p += 1;
            } else {
                break;
            }
        }
        if out.is_empty() {
            None
        } else {
            Some(String::from_utf8_lossy(&out).into_owned())
        }
    }

    fn number(&mut self, cap: usize) -> Option<String> {
        self.skip_ws();
        let mut out: Vec<u8> = Vec::new();
        while self.p < self.b.len() {
            let c = self.b[self.p];
            if c.is_ascii_alphanumeric() || c == b'x' {
                if out.len() + 1 < cap {
                    out.push(c);
                }
                self.p += 1;
            } else {
                break;
            }
        }
        if out.is_empty() {
            None
        } else {
            Some(String::from_utf8_lossy(&out).into_owned())
        }
    }
}

fn parse_memory(l: &mut LdScript, s: &mut Scan) {
    loop {
        s.skip_ws();
        s.skip_comment();
        s.skip_ws();
        if s.at_end() || s.cur() == b'}' {
            break;
        }
        let name = match s.ident(256) {
            Some(v) => v,
            None => break,
        };
        s.skip_ws();
        if !s.at_end() && s.cur() == b'(' {
            while !s.at_end() && s.cur() != b')' {
                s.p += 1;
            }
            if !s.at_end() {
                s.p += 1;
            }
        }
        s.skip_ws();
        if !s.at_end() && s.cur() == b':' {
            s.p += 1;
        }

        let mut origin = String::new();
        let mut length = String::new();
        loop {
            s.skip_ws();
            if s.at_end() || s.cur() == b'}' {
                break;
            }
            let key = match s.ident(64) {
                Some(v) => v,
                None => break,
            };
            s.skip_ws();
            if !s.at_end() && s.cur() == b'=' {
                s.p += 1;
            }
            let val = match s.number(64) {
                Some(v) => v,
                None => break,
            };
            if key.eq_ignore_ascii_case("ORIGIN") {
                origin = val;
            } else if key.eq_ignore_ascii_case("LENGTH") {
                length = val;
            }
            s.skip_ws();
            if !s.at_end() && s.cur() == b',' {
                s.p += 1;
                continue;
            }
            break;
        }
        l.regions.push(Region {
            name,
            origin,
            length,
        });
    }
}

fn parse_sections(l: &mut LdScript, s: &mut Scan) {
    loop {
        s.skip_ws();
        s.skip_comment();
        s.skip_ws();
        if s.at_end() || s.cur() == b'}' {
            break;
        }
        if s.cur() == b'.' {
            s.p += 1;
            while !s.at_end()
                && (s.cur().is_ascii_alphanumeric() || s.cur() == b'_' || s.cur() == b'.')
            {
                s.p += 1;
            }
            while !s.at_end() && s.cur() != b'{' && s.cur() != b'>' && s.cur() != b';' {
                s.p += 1;
            }
            if !s.at_end() && s.cur() == b'{' {
                let mut depth: i32 = 0;
                while !s.at_end() {
                    if s.cur() == b'{' {
                        depth += 1;
                    } else if s.cur() == b'}' {
                        depth -= 1;
                        if depth == 0 {
                            s.p += 1;
                            break;
                        }
                    }
                    s.p += 1;
                }
            }
            s.skip_ws();
            if !s.at_end() && s.cur() == b'>' {
                s.p += 1;
                if let Some(reg) = s.ident(256) {
                    l.places.push(reg);
                }
            }
            while !s.at_end() && s.cur() != b';' && s.cur() != b'}' {
                s.p += 1;
            }
            if !s.at_end() && s.cur() == b';' {
                s.p += 1;
            }
            continue;
        }
        s.p += 1;
    }
}

fn ld_parse(path: &str) -> Result<LdScript, String> {
    let data = match std::fs::read(path) {
        Ok(v) => v,
        Err(_) => return Err(format!("cannot read {}", path)),
    };
    let mut l = LdScript::default();
    let mut s = Scan { b: &data, p: 0 };
    loop {
        s.skip_ws();
        s.skip_comment();
        if s.at_end() {
            break;
        }
        let start = s.p;
        let kw = match s.ident(64) {
            Some(v) => v,
            None => {
                s.p = start + 1;
                continue;
            }
        };
        if kw.eq_ignore_ascii_case("ENTRY") {
            s.skip_ws();
            if !s.at_end() && s.cur() == b'(' {
                s.p += 1;
                if let Some(sym) = s.ident(256) {
                    l.entry = Some(sym);
                }
            }
            continue;
        }
        if kw.eq_ignore_ascii_case("MEMORY") {
            s.skip_ws();
            if !s.at_end() && s.cur() == b'{' {
                s.p += 1;
                parse_memory(&mut l, &mut s);
            }
            continue;
        }
        if kw.eq_ignore_ascii_case("SECTIONS") {
            s.skip_ws();
            if !s.at_end() && s.cur() == b'{' {
                s.p += 1;
                parse_sections(&mut l, &mut s);
            }
            continue;
        }
    }
    Ok(l)
}

fn hex_val(c: u8) -> Option<u64> {
    match c {
        b'0'..=b'9' => Some((c - b'0') as u64),
        b'a'..=b'f' => Some((c - b'a' + 10) as u64),
        b'A'..=b'F' => Some((c - b'A' + 10) as u64),
        _ => None,
    }
}

fn num(s: &str) -> u64 {
    let b = s.as_bytes();
    if b.is_empty() {
        return 0;
    }
    let mut i = 0usize;
    while i < b.len() && is_space(b[i]) {
        i += 1;
    }
    let mut neg = false;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        neg = b[i] == b'-';
        i += 1;
    }
    let mut base: u64 = 10;
    if i + 2 < b.len()
        && b[i] == b'0'
        && (b[i + 1] == b'x' || b[i + 1] == b'X')
        && b[i + 2].is_ascii_hexdigit()
    {
        base = 16;
        i += 2;
    } else if i < b.len() && b[i] == b'0' {
        base = 8;
    }
    let start = i;
    let mut v: u64 = 0;
    while i < b.len() {
        let d = match hex_val(b[i]) {
            Some(d) if d < base => d,
            _ => break,
        };
        v = v.wrapping_mul(base).wrapping_add(d);
        i += 1;
    }
    let end = if i > start { i } else { 0 };
    let mul: u64 = if end < b.len() {
        match b[end] {
            b'K' | b'k' => 1024,
            b'M' | b'm' => 1024 * 1024,
            b'G' | b'g' => 1024 * 1024 * 1024,
            _ => 1,
        }
    } else {
        1
    };
    let r = v.wrapping_mul(mul);
    if neg {
        r.wrapping_neg()
    } else {
        r
    }
}

fn emit_icf(l: &LdScript) -> Result<String, String> {
    if l.regions.is_empty() {
        return Err("cannot convert: no MEMORY region in GNU script".to_string());
    }
    let mut out = String::new();
    if let Some(e) = &l.entry {
        out.push_str(&format!("define symbol __entry = {};\n", e));
    }
    for r in l.regions.iter() {
        let o = num(&r.origin);
        out.push_str(&format!(
            "define region {} = mem:[from 0x{:x} to 0x{:x}];\n",
            r.name,
            o,
            o.wrapping_add(num(&r.length)).wrapping_sub(1)
        ));
    }
    if !l.places.is_empty() {
        for p in l.places.iter() {
            out.push_str(&format!("place in {} {{ readonly, readwrite }};\n", p));
        }
    } else {
        out.push_str(&format!(
            "place in {} {{ readonly, readwrite }};\n",
            l.regions[0].name
        ));
    }
    Ok(out)
}

fn emit_scatter(l: &LdScript) -> Result<String, String> {
    if l.regions.is_empty() {
        return Err("cannot convert: no MEMORY region in GNU script".to_string());
    }
    let mut out = String::new();
    if let Some(e) = &l.entry {
        out.push_str(&format!("ENTRY {}\n", e));
    }
    for r in l.regions.iter() {
        out.push_str(&format!(
            "  {} 0x{:x} 0x{:x} {{\n    * (+RO, +RW, +ZI)\n  }}\n",
            r.name,
            num(&r.origin),
            num(&r.length)
        ));
    }
    Ok(out)
}

fn convert(gnu_ld: &str, family: &str, out_path: &str) -> Result<(), String> {
    let l = ld_parse(gnu_ld)?;
    let mut f = match std::fs::File::create(out_path) {
        Ok(v) => v,
        Err(_) => return Err(format!("cannot write {}", out_path)),
    };
    let body = if family == "iar" {
        emit_icf(&l).map(|b| format!("// generated from {}\n{}", gnu_ld, b))
    } else if family == "armcc" {
        emit_scatter(&l).map(|b| format!("; generated from {}\n{}", gnu_ld, b))
    } else {
        Err(format!("no script converter for family '{}'", family))
    };
    match body {
        Ok(text) => {
            use std::io::Write;
            let _ = f.write_all(text.as_bytes());
            Ok(())
        }
        Err(e) => {
            drop(f);
            let _ = std::fs::remove_file(out_path);
            Err(e)
        }
    }
}

pub fn heddle_ldconv(args: &[String]) -> i32 {
    if args.len() < 5 {
        return 2;
    }
    match convert(&args[2], &args[3], &args[4]) {
        Ok(()) => 0,
        Err(e) => {
            eprintln!("heddle: ldconv: {}", e);
            1
        }
    }
}
