use std::cmp::Ordering;

use crate::glob;
use crate::model::{StarCfg, StarPlatform, StarTargetDef};
use crate::sys;

#[derive(Clone)]
enum Star {
    Nil,
    Str(String),
    Num(f64),
    Bool(bool),
    List(Vec<Star>),
    Dict(Vec<(String, Star)>),
    UserFn { params: Vec<String>, body: usize },
}

fn dict_get<'a>(d: &'a [(String, Star)], k: &str) -> Option<&'a Star> {
    d.iter().find(|(x, _)| x == k).map(|(_, v)| v)
}

fn dict_put(d: &mut Vec<(String, Star)>, k: &str, v: Star) {
    if let Some(e) = d.iter_mut().find(|(x, _)| x == k) {
        e.1 = v;
        return;
    }
    d.push((k.to_string(), v));
}

fn star_len(v: &Star) -> usize {
    match v {
        Star::List(items) => items.len(),
        Star::Dict(pairs) => pairs.len(),
        _ => 0,
    }
}

fn star_num_of(v: &Star) -> f64 {
    match v {
        Star::Num(n) => *n,
        _ => 0.0,
    }
}

fn star_as_num(v: &Star, fb: f64) -> f64 {
    match v {
        Star::Num(n) => *n,
        Star::Bool(b) => {
            if *b {
                1.0
            } else {
                0.0
            }
        }
        _ => fb,
    }
}

fn is_int(n: f64) -> bool {
    n == (n as i64) as f64
}

fn trim_zeros(s: &str) -> String {
    if !s.contains('.') {
        return s.to_string();
    }
    let t = s.trim_end_matches('0');
    t.trim_end_matches('.').to_string()
}

fn round_sig(a: f64, p: i32, x: i32) -> f64 {
    let m = 10f64.powi(p - 1 - x);
    (a * m).round() / m
}

fn fmt_g(n: f64) -> String {
    if n.is_nan() {
        return "nan".to_string();
    }
    if n.is_infinite() {
        return if n < 0.0 { "-inf" } else { "inf" }.to_string();
    }
    let neg = n.is_sign_negative();
    let a = n.abs();
    if a == 0.0 {
        return if neg { "-0" } else { "0" }.to_string();
    }
    let p = 6i32;
    let mut x = a.log10().floor() as i32;
    let mut r = round_sig(a, p, x);
    if r >= 10f64.powi(x + 1) {
        x += 1;
        r = round_sig(a, p, x);
    }
    let mut body = String::new();
    if x >= -4 && x < p {
        let dec = (p - 1 - x).max(0) as usize;
        body.push_str(&trim_zeros(&format!("{:.*}", dec, r)));
    } else {
        let m = r / 10f64.powi(x);
        body.push_str(&trim_zeros(&format!("{:.*}", (p - 1) as usize, m)));
        body.push('e');
        if x < 0 {
            body.push('-');
        } else {
            body.push('+');
        }
        body.push_str(&format!("{:02}", x.abs()));
    }
    if neg {
        format!("-{}", body)
    } else {
        body
    }
}

fn num_str(n: f64) -> String {
    if is_int(n) {
        format!("{:.0}", n)
    } else {
        fmt_g(n)
    }
}

fn as_text(v: &Star) -> String {
    match v {
        Star::Str(s) => s.clone(),
        Star::Bool(b) => {
            if *b {
                "True"
            } else {
                "False"
            }
            .to_string()
        }
        _ => num_str(star_num_of(v)),
    }
}

fn star_add(a: &mut Star, b: &Star) {
    if let (Star::Num(x), Star::Num(y)) = (&mut *a, b) {
        *x += *y;
        return;
    }
    if let (Star::List(x), Star::List(y)) = (&mut *a, b) {
        x.extend(y.iter().cloned());
        return;
    }
    let left = match a {
        Star::Str(_) | Star::Num(_) | Star::Bool(_) => Some(as_text(a)),
        _ => None,
    };
    if let Some(l) = left {
        let r = as_text(b);
        *a = Star::Str(format!("{}{}", l, r));
    }
}

fn truthy(v: &Star) -> bool {
    match v {
        Star::Bool(b) => *b,
        Star::Num(n) => *n != 0.0,
        Star::Str(s) => !s.is_empty(),
        Star::List(items) => !items.is_empty(),
        Star::Dict(pairs) => !pairs.is_empty(),
        Star::Nil => false,
        Star::UserFn { .. } => true,
    }
}

fn cmp_vals(a: &Star, b: &Star) -> i32 {
    match (a, b) {
        (Star::Num(x), Star::Num(y)) => {
            if x < y {
                -1
            } else if x > y {
                1
            } else {
                0
            }
        }
        (Star::Str(x), Star::Str(y)) => match x.as_bytes().cmp(y.as_bytes()) {
            Ordering::Less => -1,
            Ordering::Greater => 1,
            Ordering::Equal => 0,
        },
        _ => 0,
    }
}

fn same_kind(a: &Star, b: &Star) -> bool {
    matches!(
        (a, b),
        (Star::Nil, Star::Nil)
            | (Star::Str(_), Star::Str(_))
            | (Star::Num(_), Star::Num(_))
            | (Star::Bool(_), Star::Bool(_))
            | (Star::List(_), Star::List(_))
            | (Star::Dict(_), Star::Dict(_))
            | (Star::UserFn { .. }, Star::UserFn { .. })
    )
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Tt {
    Eof,
    Newline,
    Indent,
    Dedent,
    Name,
    Num,
    Str,
    Lp,
    Rp,
    Lb,
    Rb,
    Lc,
    Rc,
    Comma,
    Colon,
    Dot,
    Assign,
    Plus,
    Minus,
    Star,
    Slash,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Percent,
}

#[derive(Clone)]
struct Tok {
    t: Tt,
    s: Option<String>,
    num: f64,
    line: usize,
}

fn scan_num(s: &[u8], i: &mut usize) -> f64 {
    let start = *i;
    while *i < s.len() && s[*i].is_ascii_digit() {
        *i += 1;
    }
    if *i < s.len() && s[*i] == b'.' {
        *i += 1;
        while *i < s.len() && s[*i].is_ascii_digit() {
            *i += 1;
        }
    }
    if *i < s.len() && (s[*i] == b'e' || s[*i] == b'E') {
        let mut j = *i + 1;
        if j < s.len() && (s[j] == b'+' || s[j] == b'-') {
            j += 1;
        }
        if j < s.len() && s[j].is_ascii_digit() {
            while j < s.len() && s[j].is_ascii_digit() {
                j += 1;
            }
            *i = j;
        }
    }
    match std::str::from_utf8(&s[start..*i]) {
        Ok(t) => t.parse::<f64>().unwrap_or(0.0),
        Err(_) => 0.0,
    }
}

struct Lexer<'a> {
    s: &'a [u8],
    i: usize,
    file: &'a str,
    line: usize,
    toks: Vec<Tok>,
    indents: [i32; 64],
    depth: usize,
}

impl<'a> Lexer<'a> {
    fn push(&mut self, t: Tt, s: Option<String>, num: f64) {
        self.toks.push(Tok {
            t,
            s,
            num,
            line: self.line,
        });
    }

    fn run(&mut self) -> Result<(), String> {
        let mut at_start = true;
        let mut bdepth = 0i32;
        while self.i < self.s.len() {
            if at_start && bdepth == 0 {
                let mut q = self.i;
                let mut col = 0i32;
                while q < self.s.len() && self.s[q] == b' ' {
                    q += 1;
                    col += 1;
                }
                let blank = q < self.s.len()
                    && (self.s[q] == b'\n' || self.s[q] == b'\r' || self.s[q] == b'#');
                if q < self.s.len() && self.s[q] == b'\t' {
                    return Err(format!(
                        "{}:{}: tabs are not allowed",
                        self.file, self.line
                    ));
                }
                if blank {
                    self.i = q;
                    at_start = false;
                } else {
                    if col > self.indents[self.depth] {
                        if self.depth >= 63 {
                            return Err(format!(
                                "{}:{}: indent too deep",
                                self.file, self.line
                            ));
                        }
                        self.depth += 1;
                        self.indents[self.depth] = col;
                        self.push(Tt::Indent, None, 0.0);
                    } else {
                        while self.depth > 0 && col < self.indents[self.depth] {
                            self.depth -= 1;
                            self.push(Tt::Dedent, None, 0.0);
                        }
                        if col != self.indents[self.depth] {
                            return Err(format!("{}:{}: bad dedent", self.file, self.line));
                        }
                    }
                    self.i = q;
                    at_start = false;
                }
            }
            if self.i >= self.s.len() {
                break;
            }
            let c = self.s[self.i];
            if c == b' ' || c == b'\t' || c == b'\r' {
                self.i += 1;
                continue;
            }
            if c == b'#' {
                while self.i < self.s.len() && self.s[self.i] != b'\n' {
                    self.i += 1;
                }
                continue;
            }
            if c == b'\n' {
                if bdepth == 0 {
                    self.push(Tt::Newline, None, 0.0);
                }
                self.line += 1;
                self.i += 1;
                at_start = true;
                continue;
            }
            if c == b'"' || c == b'\'' {
                self.i += 1;
                let mut buf: Vec<u8> = Vec::new();
                while self.i < self.s.len() && self.s[self.i] != c {
                    let mut ch = self.s[self.i];
                    self.i += 1;
                    if ch == b'\\' && self.i < self.s.len() {
                        let e = self.s[self.i];
                        self.i += 1;
                        ch = if e == b'n' {
                            b'\n'
                        } else if e == b't' {
                            b'\t'
                        } else {
                            e
                        };
                    }
                    buf.push(ch);
                }
                if self.i >= self.s.len() {
                    return Err(format!(
                        "{}:{}: unterminated string",
                        self.file, self.line
                    ));
                }
                self.i += 1;
                let text = String::from_utf8_lossy(&buf).into_owned();
                self.push(Tt::Str, Some(text), 0.0);
                continue;
            }
            if c.is_ascii_digit() {
                let v = scan_num(self.s, &mut self.i);
                self.push(Tt::Num, None, v);
                continue;
            }
            if c.is_ascii_alphabetic() || c == b'_' {
                let b = self.i;
                while self.i < self.s.len()
                    && (self.s[self.i].is_ascii_alphanumeric() || self.s[self.i] == b'_')
                {
                    self.i += 1;
                }
                let text = String::from_utf8_lossy(&self.s[b..self.i]).into_owned();
                self.push(Tt::Name, Some(text), 0.0);
                continue;
            }
            self.i += 1;
            match c {
                b'(' => {
                    bdepth += 1;
                    self.push(Tt::Lp, None, 0.0);
                }
                b')' => {
                    if bdepth > 0 {
                        bdepth -= 1;
                    }
                    self.push(Tt::Rp, None, 0.0);
                }
                b'[' => {
                    bdepth += 1;
                    self.push(Tt::Lb, None, 0.0);
                }
                b']' => {
                    if bdepth > 0 {
                        bdepth -= 1;
                    }
                    self.push(Tt::Rb, None, 0.0);
                }
                b'{' => {
                    bdepth += 1;
                    self.push(Tt::Lc, None, 0.0);
                }
                b'}' => {
                    if bdepth > 0 {
                        bdepth -= 1;
                    }
                    self.push(Tt::Rc, None, 0.0);
                }
                b',' => self.push(Tt::Comma, None, 0.0),
                b':' => self.push(Tt::Colon, None, 0.0),
                b'.' => self.push(Tt::Dot, None, 0.0),
                b'+' => self.push(Tt::Plus, None, 0.0),
                b'-' => self.push(Tt::Minus, None, 0.0),
                b'*' => self.push(Tt::Star, None, 0.0),
                b'/' => self.push(Tt::Slash, None, 0.0),
                b'%' => self.push(Tt::Percent, None, 0.0),
                b'=' => {
                    if self.i < self.s.len() && self.s[self.i] == b'=' {
                        self.i += 1;
                        self.push(Tt::Eq, None, 0.0);
                    } else {
                        self.push(Tt::Assign, None, 0.0);
                    }
                }
                b'!' => {
                    if self.i < self.s.len() && self.s[self.i] == b'=' {
                        self.i += 1;
                        self.push(Tt::Ne, None, 0.0);
                    } else {
                        return Err(format!("{}:{}: unexpected '!'", self.file, self.line));
                    }
                }
                b'<' => {
                    if self.i < self.s.len() && self.s[self.i] == b'=' {
                        self.i += 1;
                        self.push(Tt::Le, None, 0.0);
                    } else {
                        self.push(Tt::Lt, None, 0.0);
                    }
                }
                b'>' => {
                    if self.i < self.s.len() && self.s[self.i] == b'=' {
                        self.i += 1;
                        self.push(Tt::Ge, None, 0.0);
                    } else {
                        self.push(Tt::Gt, None, 0.0);
                    }
                }
                _ => {
                    return Err(format!(
                        "{}:{}: unexpected '{}'",
                        self.file, self.line, c as char
                    ))
                }
            }
        }
        self.push(Tt::Newline, None, 0.0);
        while self.depth > 0 {
            self.depth -= 1;
            self.push(Tt::Dedent, None, 0.0);
        }
        self.push(Tt::Eof, None, 0.0);
        Ok(())
    }
}

fn lex(src: &[u8], file: &str) -> Result<Vec<Tok>, String> {
    let mut lx = Lexer {
        s: src,
        i: 0,
        file,
        line: 1,
        toks: Vec::new(),
        indents: [0; 64],
        depth: 0,
    };
    lx.run()?;
    Ok(lx.toks)
}

#[derive(Clone, Default)]
struct Pat {
    name: Option<String>,
    subs: Vec<Pat>,
}

struct Interp {
    toks: Vec<Tok>,
    pos: usize,
    scopes: Vec<Vec<(String, Star)>>,
    cfg: StarCfg,
    cwd: String,
    file: String,
    err: String,
    failed: bool,
    ret: Option<Star>,
}

fn kw_val<'a>(kw: &'a Star, k: &str) -> Option<&'a Star> {
    match kw {
        Star::Dict(pairs) => dict_get(pairs, k),
        _ => None,
    }
}

fn kw_str<'a>(kw: &'a Star, k: &str) -> Option<&'a str> {
    match kw_val(kw, k) {
        Some(Star::Str(s)) => Some(s.as_str()),
        _ => None,
    }
}

fn kw_dup(kw: &Star, k: &str) -> Option<String> {
    kw_str(kw, k).map(|s| s.to_string())
}

fn kw_flag(kw: &Star, k: &str) -> bool {
    match kw_val(kw, k) {
        Some(Star::Bool(b)) => *b,
        Some(Star::Num(n)) => *n != 0.0,
        _ => false,
    }
}

fn kw_set(slot: &mut Option<String>, kw: &Star, k: &str) {
    if let Some(v) = kw_str(kw, k) {
        *slot = Some(v.to_string());
    }
}

fn kw_set_s(slot: &mut String, kw: &Star, k: &str) {
    if let Some(v) = kw_str(kw, k) {
        *slot = v.to_string();
    }
}

fn obj_new(kind: &str, name: &str) -> Star {
    Star::Dict(vec![
        ("__kind".to_string(), Star::Str(kind.to_string())),
        ("name".to_string(), Star::Str(name.to_string())),
    ])
}

fn obj_name(v: &Star) -> Option<&str> {
    match v {
        Star::Str(s) => Some(s.as_str()),
        Star::Dict(pairs) => match dict_get(pairs, "name") {
            Some(Star::Str(s)) => Some(s.as_str()),
            _ => None,
        },
        _ => None,
    }
}

fn type_name(t: Option<&str>) -> &'static str {
    match t {
        None => "exe",
        Some("lib") | Some("staticlib") => "staticlib",
        Some("shared") | Some("sharedlib") => "sharedlib",
        Some("raw") => "raw",
        Some("custom") => "custom",
        _ => "exe",
    }
}

fn tg_list(kw: &Star, key: &str, out: &mut Vec<String>) {
    if let Some(Star::List(items)) = kw_val(kw, key) {
        for it in items {
            if let Star::Str(s) = it {
                out.push(s.clone());
            }
        }
    }
}

fn tg_deps(kw: &Star, t: &mut StarTargetDef) {
    if let Some(Star::List(items)) = kw_val(kw, "deps") {
        for item in items {
            let n = match obj_name(item) {
                Some(x) => x.to_string(),
                None => continue,
            };
            let is_pkg = match item {
                Star::Dict(pairs) => match dict_get(pairs, "__kind") {
                    Some(Star::Str(s)) => s == "package",
                    _ => false,
                },
                _ => false,
            };
            if is_pkg {
                t.pkgdeps.push(n);
            } else {
                t.deps.push(n);
            }
        }
    }
}

fn ref_name(kw: &Star, key: &str) -> Option<String> {
    let v = kw_val(kw, key)?;
    obj_name(v).map(|s| s.to_string())
}

fn is_unsupported(w: &str) -> bool {
    matches!(
        w,
        "while"
            | "import"
            | "class"
            | "try"
            | "with"
            | "lambda"
            | "assert"
            | "del"
            | "global"
            | "nonlocal"
            | "yield"
            | "raise"
            | "except"
            | "finally"
            | "match"
            | "async"
            | "await"
            | "exec"
            | "eval"
    )
}

impl Interp {
    fn peek(&self) -> &Tok {
        &self.toks[self.pos]
    }

    fn peek2(&self) -> &Tok {
        if self.pos + 1 < self.toks.len() {
            &self.toks[self.pos + 1]
        } else {
            &self.toks[self.toks.len() - 1]
        }
    }

    fn tt(&self) -> Tt {
        self.toks[self.pos].t
    }

    fn next_tok(&mut self) -> Tok {
        if self.toks[self.pos].t != Tt::Eof {
            self.pos += 1;
        }
        let i = if self.pos > 0 { self.pos - 1 } else { 0 };
        self.toks[i].clone()
    }

    fn accept(&mut self, t: Tt) -> bool {
        if self.toks[self.pos].t != t {
            return false;
        }
        self.next_tok();
        true
    }

    fn fail(&mut self, msg: String) {
        if !self.failed {
            self.err = msg;
        }
        self.failed = true;
    }

    fn scope_push(&mut self) -> bool {
        if self.scopes.len() >= 64 {
            return false;
        }
        self.scopes.push(Vec::new());
        true
    }

    fn scope_pop(&mut self) {
        if self.scopes.is_empty() {
            return;
        }
        self.scopes.pop();
    }

    fn var_get(&self, name: &str) -> Option<&Star> {
        for sc in self.scopes.iter().rev() {
            if let Some(e) = sc.iter().find(|(k, _)| k == name) {
                return Some(&e.1);
            }
        }
        None
    }

    fn var_set(&mut self, name: &str, val: Star) {
        for sc in self.scopes.iter_mut().rev() {
            if let Some(e) = sc.iter_mut().find(|(k, _)| k == name) {
                e.1 = val;
                return;
            }
        }
        let last = match self.scopes.last_mut() {
            Some(s) => s,
            None => return,
        };
        if last.len() >= 512 {
            return;
        }
        last.push((name.to_string(), val));
    }

    fn eval_expr(&mut self) -> Star {
        if self.failed {
            return Star::Nil;
        }
        self.eval_cmp()
    }

    fn eval_cmp(&mut self) -> Star {
        let mut v = self.eval_add();
        loop {
            let t = self.tt();
            if !matches!(
                t,
                Tt::Eq | Tt::Ne | Tt::Lt | Tt::Le | Tt::Gt | Tt::Ge
            ) {
                return v;
            }
            self.next_tok();
            let r = self.eval_add();
            let c = cmp_vals(&v, &r);
            let o = match t {
                Tt::Eq => c == 0 && same_kind(&v, &r),
                Tt::Ne => !(c == 0 && same_kind(&v, &r)),
                Tt::Lt => c < 0,
                Tt::Le => c <= 0,
                Tt::Gt => c > 0,
                _ => c >= 0,
            };
            v = Star::Bool(o);
        }
    }

    fn eval_add(&mut self) -> Star {
        let mut v = self.eval_mul();
        loop {
            let t = self.tt();
            if t != Tt::Plus && t != Tt::Minus {
                return v;
            }
            self.next_tok();
            let r = self.eval_mul();
            if t == Tt::Plus {
                star_add(&mut v, &r);
            } else {
                let o = star_as_num(&v, 0.0) - star_as_num(&r, 0.0);
                v = Star::Num(o);
            }
        }
    }

    fn eval_mul(&mut self) -> Star {
        let mut v = self.eval_unary();
        loop {
            let t = self.tt();
            if t != Tt::Star && t != Tt::Slash && t != Tt::Percent {
                return v;
            }
            self.next_tok();
            let r = self.eval_unary();
            let a = star_as_num(&v, 0.0);
            let b = star_as_num(&r, 0.0);
            let o = match t {
                Tt::Star => a * b,
                Tt::Slash => {
                    if b != 0.0 {
                        a / b
                    } else {
                        0.0
                    }
                }
                _ => {
                    if b != 0.0 {
                        let q = (a / b) as i64 as f64;
                        a - b * q
                    } else {
                        0.0
                    }
                }
            };
            v = Star::Num(o);
        }
    }

    fn eval_unary(&mut self) -> Star {
        if self.accept(Tt::Minus) {
            let v = self.eval_unary();
            if let Star::Num(n) = v {
                return Star::Num(-n);
            }
            return v;
        }
        self.eval_primary()
    }

    fn eval_primary(&mut self) -> Star {
        let t = self.peek().clone();
        if t.t == Tt::Num {
            self.next_tok();
            return Star::Num(t.num);
        }
        if t.t == Tt::Str {
            self.next_tok();
            return Star::Str(t.s.clone().unwrap_or_default());
        }
        if t.t == Tt::Lb {
            self.next_tok();
            let mut items: Vec<Star> = Vec::new();
            if self.accept(Tt::Rb) {
                return self.postfix(Star::List(items));
            }
            loop {
                let v = self.eval_expr();
                items.push(v);
                if self.accept(Tt::Comma) {
                    if self.tt() == Tt::Rb {
                        self.next_tok();
                        break;
                    }
                    continue;
                }
                self.accept(Tt::Rb);
                break;
            }
            return self.postfix(Star::List(items));
        }
        if t.t == Tt::Lc {
            self.next_tok();
            let mut pairs: Vec<(String, Star)> = Vec::new();
            if self.accept(Tt::Rc) {
                return self.postfix(Star::Dict(pairs));
            }
            loop {
                let k = self.eval_expr();
                self.accept(Tt::Colon);
                let v = self.eval_expr();
                let kb = match &k {
                    Star::Str(s) => s.clone(),
                    _ => String::new(),
                };
                dict_put(&mut pairs, &kb, v);
                if self.accept(Tt::Comma) {
                    if self.tt() == Tt::Rc {
                        self.next_tok();
                        break;
                    }
                    continue;
                }
                self.accept(Tt::Rc);
                break;
            }
            return self.postfix(Star::Dict(pairs));
        }
        if t.t == Tt::Lp {
            self.next_tok();
            let v = self.eval_expr();
            if self.tt() == Tt::Comma {
                let mut items: Vec<Star> = vec![v];
                while self.accept(Tt::Comma) {
                    if self.tt() == Tt::Rp {
                        break;
                    }
                    let x = self.eval_expr();
                    items.push(x);
                }
                self.accept(Tt::Rp);
                return self.postfix(Star::List(items));
            }
            self.accept(Tt::Rp);
            return v;
        }
        if t.t == Tt::Name {
            self.next_tok();
            let name = t.s.clone().unwrap_or_default();
            if name == "True" {
                return Star::Bool(true);
            }
            if name == "False" {
                return Star::Bool(false);
            }
            if name == "None" {
                return Star::Nil;
            }
            if self.tt() == Tt::Lp {
                self.next_tok();
                let (args, kw) = self.eval_args();
                let r = self.eval_call(&name, &args, &kw);
                return self.postfix(r);
            }
            let v = self.var_get(&name).cloned().unwrap_or(Star::Nil);
            return self.postfix(v);
        }
        Star::Nil
    }

    fn postfix(&mut self, mut v: Star) -> Star {
        loop {
            if self.accept(Tt::Dot) {
                if self.tt() != Tt::Name {
                    return v;
                }
                let m = self.peek().s.clone().unwrap_or_default();
                self.next_tok();
                if self.accept(Tt::Lp) {
                    self.accept(Tt::Rp);
                }
                let newv: Option<Star> = match &v {
                    Star::Dict(pairs) => {
                        if m == "keys" {
                            Some(Star::List(
                                pairs
                                    .iter()
                                    .map(|(k, _)| Star::Str(k.clone()))
                                    .collect::<Vec<Star>>(),
                            ))
                        } else if m == "items" {
                            Some(Star::List(
                                pairs
                                    .iter()
                                    .map(|(k, val)| {
                                        Star::List(vec![Star::Str(k.clone()), val.clone()])
                                    })
                                    .collect::<Vec<Star>>(),
                            ))
                        } else if m == "values" {
                            Some(Star::List(
                                pairs.iter().map(|(_, val)| val.clone()).collect::<Vec<Star>>(),
                            ))
                        } else {
                            None
                        }
                    }
                    _ => None,
                };
                if let Some(nv) = newv {
                    v = nv;
                }
                continue;
            }
            if self.tt() == Tt::Lb {
                self.next_tok();
                v = self.eval_index(&v);
                continue;
            }
            break;
        }
        v
    }

    fn eval_index(&mut self, base: &Star) -> Star {
        let bn = star_len(base) as i32;
        if self.tt() == Tt::Colon {
            self.next_tok();
            let mut hi = bn;
            if self.tt() != Tt::Rb {
                let hv = self.eval_expr();
                hi = star_as_num(&hv, bn as f64) as i32;
            }
            self.accept(Tt::Rb);
            let mut out: Vec<Star> = Vec::new();
            if let Star::List(items) = base {
                let mut i: i32 = 0;
                while i < hi && (i as usize) < items.len() {
                    out.push(items[i as usize].clone());
                    i += 1;
                }
            }
            return Star::List(out);
        }

        let idx = self.eval_expr();
        if self.accept(Tt::Colon) {
            let lo = star_as_num(&idx, 0.0) as i32;
            let mut hi = bn;
            if self.tt() != Tt::Rb {
                let hv = self.eval_expr();
                hi = star_as_num(&hv, bn as f64) as i32;
            }
            self.accept(Tt::Rb);
            let mut out: Vec<Star> = Vec::new();
            if let Star::List(items) = base {
                let mut i: i32 = lo.max(0);
                while i < hi && (i as usize) < items.len() {
                    out.push(items[i as usize].clone());
                    i += 1;
                }
            }
            return Star::List(out);
        }

        self.accept(Tt::Rb);
        match base {
            Star::Dict(pairs) => match &idx {
                Star::Str(k) => dict_get(pairs, k).cloned().unwrap_or(Star::Nil),
                _ => Star::Nil,
            },
            Star::List(items) => {
                let i = star_as_num(&idx, 0.0) as i64;
                if i >= 0 && (i as usize) < items.len() {
                    items[i as usize].clone()
                } else {
                    Star::Nil
                }
            }
            _ => Star::Nil,
        }
    }

    fn eval_args(&mut self) -> (Star, Star) {
        let mut args: Vec<Star> = Vec::new();
        let mut kw: Vec<(String, Star)> = Vec::new();
        if self.accept(Tt::Rp) {
            return (Star::List(args), Star::Dict(kw));
        }
        loop {
            if self.tt() == Tt::Name && self.peek2().t == Tt::Assign {
                let key = self.peek().s.clone().unwrap_or_default();
                self.next_tok();
                self.next_tok();
                let v = self.eval_expr();
                dict_put(&mut kw, &key, v);
            } else {
                let v = self.eval_expr();
                args.push(v);
            }
            if self.accept(Tt::Comma) {
                continue;
            }
            self.accept(Tt::Rp);
            break;
        }
        (Star::List(args), Star::Dict(kw))
    }

    fn eval_call(&mut self, name: &str, args: &Star, kw: &Star) -> Star {
        if let Some(Star::UserFn { params, body }) = self.var_get(name).cloned() {
            if !self.scope_push() {
                self.fail("too deep".to_string());
                return Star::Nil;
            }
            for (i, p) in params.iter().enumerate() {
                let v = match args {
                    Star::List(items) => items.get(i).cloned().unwrap_or(Star::Nil),
                    _ => Star::Nil,
                };
                self.var_set(p, v);
            }
            let save = self.pos;
            self.pos = body;
            let mut r = self.eval_block(0);
            if let Some(rv) = self.ret.take() {
                r = rv;
            }
            self.pos = save;
            self.scope_pop();
            return r;
        }
        self.call_builtin(name, args, kw)
    }

    fn call_builtin(&mut self, name: &str, args: &Star, kw: &Star) -> Star {
        match name {
            "len" => Star::Num(star_len(match args {
                Star::List(items) => items.first().unwrap_or(&Star::Nil),
                _ => &Star::Nil,
            }) as f64),
            "range" => {
                let items = match args {
                    Star::List(v) => v,
                    _ => return Star::List(Vec::new()),
                };
                let mut lo = 0i32;
                let mut hi = 0i32;
                if items.len() == 1 {
                    hi = star_as_num(&items[0], 0.0) as i32;
                } else if items.len() >= 2 {
                    lo = star_as_num(&items[0], 0.0) as i32;
                    hi = star_as_num(&items[1], 0.0) as i32;
                }
                let mut out: Vec<Star> = Vec::new();
                let mut i = lo;
                while i < hi {
                    out.push(Star::Num(i as f64));
                    i += 1;
                }
                Star::List(out)
            }
            "str" => {
                let v = match args {
                    Star::List(items) => items.first().unwrap_or(&Star::Nil),
                    _ => &Star::Nil,
                };
                match v {
                    Star::Str(_) => v.clone(),
                    Star::Bool(b) => Star::Str(if *b { "True" } else { "False" }.to_string()),
                    Star::Num(n) => Star::Str(num_str(*n)),
                    Star::Nil => Star::Str("None".to_string()),
                    _ => Star::Str(String::new()),
                }
            }
            "glob" => self.b_glob(args),
            "build" => {
                kw_set(&mut self.cfg.build_dir, kw, "dir");
                kw_set(&mut self.cfg.toolchain, kw, "toolchain");
                kw_set(&mut self.cfg.store, kw, "store");
                Star::Nil
            }
            "project" => {
                kw_set(&mut self.cfg.project_name, kw, "name");
                kw_set(&mut self.cfg.build_dir, kw, "build_dir");
                kw_set(&mut self.cfg.toolchain, kw, "default_toolchain");
                kw_set(&mut self.cfg.store, kw, "store");
                Star::Nil
            }
            "target_config" => {
                kw_set_s(&mut self.cfg.arch, kw, "arch");
                kw_set_s(&mut self.cfg.abi, kw, "abi");
                kw_set_s(&mut self.cfg.flt, kw, "float");
                kw_set_s(&mut self.cfg.sysroot, kw, "sysroot");
                Star::Nil
            }
            "toolchain" => {
                let name = kw_dup(kw, "name");
                let cc = kw_dup(kw, "cc").unwrap_or_default();
                let base = kw_dup(kw, "based_on").unwrap_or_default();
                if let Some(n) = &name {
                    self.cfg.tc_name.push(n.clone());
                    self.cfg.tc_cc.push(cc);
                    self.cfg.tc_based.push(base);
                }
                obj_new("toolchain", name.as_deref().unwrap_or(""))
            }
            "package" => {
                let name = kw_dup(kw, "name");
                let ver = kw_dup(kw, "version").unwrap_or_else(|| "latest".to_string());
                let src = kw_dup(kw, "source").unwrap_or_default();
                if let Some(n) = &name {
                    self.cfg.dep_name.push(n.clone());
                    self.cfg.dep_ver.push(ver);
                    self.cfg.dep_src.push(src);
                }
                obj_new("package", name.as_deref().unwrap_or(""))
            }
            "platform" => {
                let name = match kw_dup(kw, "name") {
                    Some(n) => n,
                    None => {
                        self.fail("platform(): missing name".to_string());
                        return Star::Nil;
                    }
                };
                let p = StarPlatform {
                    name: name.clone(),
                    arch: kw_dup(kw, "arch").unwrap_or_default(),
                    abi: kw_dup(kw, "abi").unwrap_or_default(),
                    flt: kw_dup(kw, "float").unwrap_or_default(),
                    sysroot: kw_dup(kw, "sysroot").unwrap_or_default(),
                    toolchain: kw_dup(kw, "toolchain").unwrap_or_default(),
                };
                self.cfg.platforms.push(p);
                obj_new("platform", &name)
            }
            "target" => {
                let name = match kw_dup(kw, "name") {
                    Some(n) => n,
                    None => {
                        self.fail("target(): missing name".to_string());
                        return Star::Nil;
                    }
                };
                let mut t = StarTargetDef {
                    name,
                    kind: Some(type_name(kw_str(kw, "type")).to_string()),
                    ..StarTargetDef::default()
                };
                tg_list(kw, "src", &mut t.src);
                tg_list(kw, "inc", &mut t.inc);
                tg_deps(kw, &mut t);
                tg_list(kw, "cflags", &mut t.cflags);
                tg_list(kw, "ldflags", &mut t.ldflags);
                t.ldscript = kw_dup(kw, "linker_script");
                t.entry = kw_dup(kw, "entry");
                t.out = kw_dup(kw, "out");
                t.platform = ref_name(kw, "platform");
                t.start_group = kw_flag(kw, "start_group");
                t.whole_archive = kw_flag(kw, "whole_archive");
                self.cfg.targets.push(t);
                Star::Nil
            }
            "install" => {
                let name = match kw_dup(kw, "target") {
                    Some(n) => n,
                    None => {
                        self.fail("install(): missing target".to_string());
                        return Star::Nil;
                    }
                };
                let mut found: Option<usize> = None;
                for i in 0..self.cfg.targets.len() {
                    if self.cfg.targets[i].name == name {
                        found = Some(i);
                        break;
                    }
                }
                let i = match found {
                    Some(i) => i,
                    None => {
                        self.fail("install(): unknown target".to_string());
                        return Star::Nil;
                    }
                };
                let t = &mut self.cfg.targets[i];
                tg_list(kw, "bin", &mut t.i_bin);
                tg_list(kw, "lib", &mut t.i_lib);
                tg_list(kw, "include", &mut t.i_include);
                tg_list(kw, "share", &mut t.i_share);
                tg_list(kw, "etc", &mut t.i_etc);
                t.i_rootfs = kw_dup(kw, "rootfs");
                Star::Nil
            }
            _ => {
                let f = self.file.clone();
                self.fail(format!("{}: unknown function '{}'", f, name));
                Star::Nil
            }
        }
    }

    fn b_glob(&self, args: &Star) -> Star {
        let mut out: Vec<Star> = Vec::new();
        let base = if self.cwd.is_empty() {
            "."
        } else {
            self.cwd.as_str()
        };
        let items = match args {
            Star::List(v) => v,
            _ => return Star::List(out),
        };
        for it in items {
            let pat = match it {
                Star::Str(s) => s.as_str(),
                _ => continue,
            };
            let bytes = pat.as_bytes();
            let sc = match bytes.iter().position(|&b| b == b'*' || b == b'?') {
                Some(i) => i,
                None => bytes.len(),
            };
            let mut dir: Option<String> = None;
            let mut gp: &str = pat;
            if sc < bytes.len() {
                let mut slash: Option<usize> = None;
                for (k, &b) in bytes.iter().enumerate() {
                    if k >= sc {
                        break;
                    }
                    if b == b'/' {
                        slash = Some(k);
                    }
                }
                if let Some(sl) = slash {
                    dir = Some(format!("{}/{}", base, &pat[..sl]));
                    gp = &pat[sl + 1..];
                }
            }
            let d = match &dir {
                Some(x) => x.as_str(),
                None => base,
            };
            for m in glob::glob_dir(d, gp) {
                match &dir {
                    Some(dd) => out.push(Star::Str(format!("{}/{}", dd, m))),
                    None => out.push(Star::Str(m)),
                }
            }
        }
        Star::List(out)
    }

    fn skip_block(&mut self) {
        self.accept(Tt::Newline);
        let mut depth = 0i32;
        while self.tt() != Tt::Eof {
            if self.tt() == Tt::Indent {
                depth += 1;
                self.next_tok();
            } else if self.tt() == Tt::Dedent {
                self.next_tok();
                depth -= 1;
                if depth <= 0 {
                    break;
                }
            } else {
                self.next_tok();
            }
        }
    }

    fn skip_line(&mut self) {
        while self.tt() != Tt::Newline && self.tt() != Tt::Eof && self.tt() != Tt::Dedent {
            self.next_tok();
        }
        self.accept(Tt::Newline);
    }

    fn parse_pattern(&mut self, p: &mut Pat) -> bool {
        if self.accept(Tt::Lp) {
            loop {
                let mut sub = Pat::default();
                if !self.parse_pattern(&mut sub) {
                    return false;
                }
                p.subs.push(sub);
                if self.accept(Tt::Comma) {
                    continue;
                }
                self.accept(Tt::Rp);
                break;
            }
            return true;
        }
        if self.tt() == Tt::Name {
            p.name = self.peek().s.clone();
            self.next_tok();
            return true;
        }
        false
    }

    fn bind_pattern(&mut self, p: &Pat, v: &Star) {
        if let Some(name) = &p.name {
            self.var_set(name, v.clone());
            return;
        }
        if let Star::List(items) = v {
            for (i, sub) in p.subs.iter().enumerate() {
                let item = items.get(i).cloned().unwrap_or(Star::Nil);
                self.bind_pattern(sub, &item);
            }
        }
    }

    fn eval_block(&mut self, nl: i32) -> Star {
        if nl > 0 {
            self.accept(Tt::Newline);
        }
        if nl >= 0 && !self.accept(Tt::Indent) {
            let f = self.file.clone();
            self.fail(format!("{}: expected indented block", f));
            return Star::Nil;
        }

        let mut last = Star::Nil;
        while !self.failed && self.ret.is_none() {
            let t = self.tt();
            if t == Tt::Dedent || t == Tt::Eof {
                break;
            }
            if self.accept(Tt::Newline) {
                continue;
            }
            let tok = self.peek().clone();

            if tok.t == Tt::Name && tok.s.as_deref() == Some("def") {
                self.next_tok();
                let nm = self.next_tok();
                if nm.t != Tt::Name {
                    self.fail("bad def".to_string());
                    break;
                }
                let mut params: Vec<String> = Vec::new();
                self.accept(Tt::Lp);
                if !self.accept(Tt::Rp) {
                    loop {
                        let p = self.next_tok();
                        if p.t == Tt::Name && params.len() < 32 {
                            params.push(p.s.clone().unwrap_or_default());
                        }
                        if self.accept(Tt::Comma) {
                            continue;
                        }
                        self.accept(Tt::Rp);
                        break;
                    }
                }
                self.accept(Tt::Colon);
                self.accept(Tt::Newline);
                let body = self.pos;
                self.accept(Tt::Indent);
                let mut depth = 1i32;
                while depth > 0 && self.tt() != Tt::Eof {
                    if self.accept(Tt::Indent) {
                        depth += 1;
                    } else if self.accept(Tt::Dedent) {
                        depth -= 1;
                    } else {
                        self.next_tok();
                    }
                }
                let name = nm.s.clone().unwrap_or_default();
                self.var_set(&name, Star::UserFn { params, body });
                continue;
            }

            if tok.t == Tt::Name && tok.s.as_deref() == Some("if") {
                self.next_tok();
                let mut done = false;
                loop {
                    if done {
                        self.skip_block();
                    } else {
                        let c = self.eval_expr();
                        self.accept(Tt::Colon);
                        if truthy(&c) {
                            last = self.eval_block(1);
                            done = true;
                        } else {
                            self.skip_block();
                        }
                    }
                    if self.tt() != Tt::Name {
                        break;
                    }
                    let w = self.peek().s.clone().unwrap_or_default();
                    if w == "elif" {
                        self.next_tok();
                        continue;
                    }
                    if w == "else" {
                        self.next_tok();
                        self.accept(Tt::Colon);
                        if !done {
                            last = self.eval_block(1);
                        } else {
                            self.skip_block();
                        }
                    }
                    break;
                }
                continue;
            }

            if tok.t == Tt::Name && tok.s.as_deref() == Some("for") {
                self.next_tok();
                let mut pat = Pat::default();
                let mut bad = false;
                loop {
                    let mut one = Pat::default();
                    if !self.parse_pattern(&mut one) {
                        self.fail("for: bad loop variable".to_string());
                        bad = true;
                        break;
                    }
                    pat.subs.push(one);
                    if self.accept(Tt::Comma) {
                        continue;
                    }
                    break;
                }
                if bad {
                    continue;
                }
                if pat.subs.len() == 1 && pat.subs[0].name.is_some() {
                    pat.name = pat.subs[0].name.clone();
                    pat.subs.clear();
                }
                self.next_tok();
                let it = self.eval_expr();
                self.accept(Tt::Colon);
                self.accept(Tt::Newline);
                if self.tt() != Tt::Indent {
                    self.fail("for: need block".to_string());
                    break;
                }
                let body = self.pos;
                let mut depth = 1i32;
                self.accept(Tt::Indent);
                while depth > 0 && self.tt() != Tt::Eof {
                    if self.accept(Tt::Indent) {
                        depth += 1;
                    } else if self.accept(Tt::Dedent) {
                        depth -= 1;
                    } else {
                        self.next_tok();
                    }
                }
                let count = star_len(&it);
                let is_dict = matches!(it, Star::Dict(_));
                let iterable = is_dict || matches!(it, Star::List(_));
                if iterable {
                    let mut i = 0usize;
                    while i < count && !self.failed && self.ret.is_none() {
                        self.scope_push();
                        if let Star::Dict(pairs) = &it {
                            let key = pairs[i].0.clone();
                            let val = pairs[i].1.clone();
                            self.bind_pattern(&pat, &Star::Str(key.clone()));
                            if !pat.subs.is_empty() || pat.name.is_none() {
                                let pair = Star::List(vec![Star::Str(key.clone()), val]);
                                self.bind_pattern(&pat, &pair);
                            } else if let Some(pn) = &pat.name {
                                self.var_set(pn, Star::Str(key));
                            }
                        } else if let Star::List(items) = &it {
                            self.bind_pattern(&pat, &items[i]);
                        }
                        let save = self.pos;
                        self.pos = body;
                        last = self.eval_block(0);
                        self.pos = save;
                        self.scope_pop();
                        i += 1;
                    }
                }
                continue;
            }

            if tok.t == Tt::Name && tok.s.as_deref() == Some("return") {
                self.next_tok();
                if self.tt() == Tt::Newline {
                    self.ret = Some(Star::Nil);
                } else {
                    let v = self.eval_expr();
                    self.ret = Some(v);
                }
                self.skip_line();
                break;
            }

            if tok.t == Tt::Name {
                let w = tok.s.clone().unwrap_or_default();
                if is_unsupported(&w) {
                    let f = self.file.clone();
                    self.fail(format!("{}:{}: '{}' is not supported", f, tok.line, w));
                    break;
                }
                if w == "pass" {
                    self.next_tok();
                    self.skip_line();
                    continue;
                }
                if w == "break" {
                    self.next_tok();
                    self.skip_line();
                    continue;
                }
            }

            if tok.t == Tt::Name && self.peek2().t == Tt::Assign {
                let name = tok.s.clone().unwrap_or_default();
                self.next_tok();
                self.next_tok();
                let v = self.eval_expr();
                self.var_set(&name, v);
                self.skip_line();
                continue;
            }

            self.eval_expr();
            self.skip_line();
        }

        self.accept(Tt::Dedent);
        last
    }
}

pub fn run(path: &str) -> Result<StarCfg, String> {
    let src = match sys::read_bytes(path) {
        Some(b) => b,
        None => return Err(format!("cannot read {}", path)),
    };
    let toks = lex(&src, path)?;
    let cfg = StarCfg {
        root: sys::dir_of(path),
        ..StarCfg::default()
    };
    let cwd = cfg.root.clone();
    let mut it = Interp {
        toks,
        pos: 0,
        scopes: Vec::new(),
        cfg,
        cwd,
        file: path.to_string(),
        err: String::new(),
        failed: false,
        ret: None,
    };
    it.scope_push();
    it.eval_block(-1);
    if it.failed {
        return Err(it.err);
    }
    Ok(it.cfg)
}
