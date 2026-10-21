use crate::sys;

pub fn has_wild(s: &str) -> bool {
    s.contains('*') || s.contains('?')
}

fn seg_match(pat: &[u8], name: &[u8]) -> bool {
    let mut pi = 0usize;
    let mut ni = 0usize;
    let mut star: Option<usize> = None;
    let mut mark = 0usize;
    while ni < name.len() {
        if pi < pat.len() && (pat[pi] == b'?' || pat[pi] == name[ni]) {
            pi += 1;
            ni += 1;
        } else if pi < pat.len() && pat[pi] == b'*' {
            star = Some(pi);
            pi += 1;
            mark = ni;
        } else if let Some(s) = star {
            pi = s + 1;
            mark += 1;
            ni = mark;
        } else {
            return false;
        }
    }
    while pi < pat.len() && pat[pi] == b'*' {
        pi += 1;
    }
    pi == pat.len()
}

fn split_once_slash(s: &str) -> (&str, Option<&str>) {
    match s.find('/') {
        Some(i) => (&s[..i], Some(&s[i + 1..])),
        None => (s, None),
    }
}

fn match_segs(pat: &str, name: &str) -> bool {
    if pat.is_empty() {
        return name.is_empty();
    }
    let (pseg, prest) = split_once_slash(pat);
    let (nseg, nrest) = split_once_slash(name);
    let pend = prest.is_none();
    let nend = nrest.is_none();

    if pseg == "**" {
        let r = prest.unwrap_or("");
        if match_segs(r, name) {
            return true;
        }
        return !nend && match_segs(pat, nrest.unwrap());
    }

    if !seg_match(pseg.as_bytes(), nseg.as_bytes()) {
        return false;
    }
    if pend {
        return nend;
    }
    if nend {
        return false;
    }
    match_segs(prest.unwrap(), nrest.unwrap())
}

pub fn glob_match(pat: &str, name: &str) -> bool {
    match_segs(pat, name)
}

fn walk(base: &str, rel: &str, pat: &str, out: &mut Vec<String>) {
    let dir = if rel.is_empty() {
        base.to_string()
    } else {
        sys::join(base, rel)
    };
    let rd = match std::fs::read_dir(&dir) {
        Ok(r) => r,
        Err(_) => return,
    };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if name == "." || name == ".." {
            continue;
        }
        let r = if rel.is_empty() {
            name.clone()
        } else {
            format!("{}/{}", rel, name)
        };
        let full = sys::join(&dir, &name);
        if sys::is_dir(&full) {
            walk(base, &r, pat, out);
        } else if glob_match(pat, &r) {
            out.push(r);
        }
    }
}

pub fn glob_dir(dir: &str, pat: &str) -> Vec<String> {
    let mut out = Vec::new();
    walk(dir, "", pat, &mut out);
    out
}
