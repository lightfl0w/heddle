use crate::hash::{hash_text, HASH_FNV_OFFSET};
use std::collections::HashMap;

#[derive(Default, Clone)]
pub struct Node {
    pub argv: Vec<String>,
    pub deps: Vec<usize>,
    pub ndeps_static: usize,
    pub rdeps: Vec<usize>,
    pub indeg: usize,
    pub ins: Vec<String>,
    pub outs: Vec<String>,
    pub incdirs: Vec<String>,
    pub dyns: Vec<String>,
    pub scan_hash: u64,
    pub cmd_hash: u64,
}

pub struct Graph {
    pub nodes: Vec<Node>,
}

fn is_ws(c: u8) -> bool {
    c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' || c == 0x0b || c == 0x0c
}

fn atoi(s: &str) -> i64 {
    let t = s.trim_start();
    let mut sign = 1i64;
    let mut i = 0usize;
    let b = t.as_bytes();
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

pub fn split_ws(s: &str) -> Vec<String> {
    let b = s.as_bytes();
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < b.len() {
        while i < b.len() && is_ws(b[i]) {
            i += 1;
        }
        if i >= b.len() {
            break;
        }
        let mut buf: Vec<u8> = Vec::new();
        while i < b.len() && !is_ws(b[i]) {
            let q = b[i];
            if q != b'"' && q != b'\'' {
                if buf.len() + 1 < 8192 {
                    buf.push(b[i]);
                }
                i += 1;
                continue;
            }
            i += 1;
            while i < b.len() && b[i] != q {
                if b[i] == b'\\' && i + 1 < b.len() {
                    i += 1;
                }
                if buf.len() + 1 < 8192 {
                    buf.push(b[i]);
                }
                i += 1;
            }
            if i < b.len() && b[i] == q {
                i += 1;
            }
        }
        out.push(String::from_utf8_lossy(&buf).into_owned());
    }
    out
}

fn find_unquoted(s: &str, c: u8) -> Option<usize> {
    let b = s.as_bytes();
    let mut q = 0u8;
    for (i, &ch) in b.iter().enumerate() {
        if q != 0 {
            if ch == q {
                q = 0;
            }
            continue;
        }
        if ch == b'"' || ch == b'\'' {
            q = ch;
            continue;
        }
        if ch == c {
            return Some(i);
        }
    }
    None
}

fn looks_like_path(s: &str) -> bool {
    !s.starts_with('-') && (s.contains('.') || s.contains('/'))
}

fn is_ar(tool: &str) -> bool {
    let b = crate::sys::base_name(tool);
    b == "ar" || b == "ar.exe" || (b.len() > 3 && b.ends_with("-ar"))
}

fn ar_output(nd: &Node) -> Option<String> {
    if nd.argv.len() < 3 || !is_ar(&nd.argv[0]) {
        return None;
    }
    let mut flags = nd.argv[1].as_str();
    if flags.starts_with('-') {
        flags = &flags[1..];
    }
    if flags != "rcs" && flags != "rc" && flags != "r" {
        return None;
    }
    let a = &nd.argv[2];
    if looks_like_path(a) {
        Some(a.clone())
    } else {
        None
    }
}

fn is_own_output(nd: &Node, s: &str) -> bool {
    nd.outs.iter().any(|o| o == s)
}

fn list_add_words(v: &mut Vec<String>, words: &str, skip_outs: bool, nd: &Node) {
    for t in split_ws(words) {
        if skip_outs && is_own_output(nd, &t) {
            continue;
        }
        v.push(t);
    }
}

fn node_scan(nd: &mut Node) {
    nd.cmd_hash = HASH_FNV_OFFSET;
    for a in &nd.argv {
        nd.cmd_hash = hash_text(nd.cmd_hash, a);
    }

    let argc = nd.argv.len();
    let mut i = 1usize;
    while i < argc {
        if nd.argv[i] == "-o" && i + 1 < argc {
            let v = nd.argv[i + 1].clone();
            nd.outs.push(v);
            i += 1;
        }
        i += 1;
    }
    if let Some(a) = ar_output(nd) {
        if !nd.outs.contains(&a) {
            nd.outs.push(a);
        }
    }

    let mut i = 1usize;
    while i < argc {
        let arg = nd.argv[i].clone();
        if arg == "-I" && i + 1 < argc {
            nd.incdirs.push(nd.argv[i + 1].clone());
            i += 2;
            continue;
        }
        if arg.starts_with("-I") && arg.len() > 2 {
            nd.incdirs.push(arg[2..].to_string());
            i += 1;
            continue;
        }
        if arg == "-o" {
            i += 2;
            continue;
        }
        if !looks_like_path(&arg) {
            i += 1;
            continue;
        }
        if is_own_output(nd, &arg) {
            i += 1;
            continue;
        }
        nd.ins.push(arg);
        i += 1;
    }
}

fn build_rdeps(g: &mut Graph) -> Result<(), String> {
    let n = g.nodes.len();
    let mut rcap = vec![0usize; n];
    for i in 0..n {
        let deps = g.nodes[i].deps.clone();
        for d in deps {
            rcap[d] += 1;
        }
    }
    for (i, nd) in g.nodes.iter_mut().enumerate() {
        nd.rdeps = Vec::with_capacity(rcap[i]);
        nd.rdeps.clear();
    }
    for i in 0..n {
        let deps = g.nodes[i].deps.clone();
        for d in deps {
            g.nodes[d].rdeps.push(i);
        }
    }
    Ok(())
}

fn detect_cycle(g: &Graph) -> Result<(), String> {
    let n = g.nodes.len();
    if n == 0 {
        return Ok(());
    }
    let mut indeg: Vec<usize> = g.nodes.iter().map(|x| x.indeg).collect();
    let mut queue: Vec<usize> = Vec::with_capacity(n);
    for (i, &d) in indeg.iter().enumerate() {
        if d == 0 {
            queue.push(i);
        }
    }
    let mut qh = 0usize;
    let mut visited = 0usize;
    while qh < queue.len() {
        let u = queue[qh];
        qh += 1;
        visited += 1;
        for k in 0..g.nodes[u].rdeps.len() {
            let v = g.nodes[u].rdeps[k];
            if indeg[v] > 0 {
                indeg[v] -= 1;
            }
            if indeg[v] == 0 {
                queue.push(v);
            }
        }
    }
    if visited < n {
        return Err("cycle detected in DAG".to_string());
    }
    Ok(())
}

fn node_add_dep(nd: &mut Node, d: usize) {
    if !nd.deps.contains(&d) {
        nd.deps.push(d);
    }
}

pub fn finalize(g: &mut Graph) -> Result<(), String> {
    let n = g.nodes.len();
    let mut producers: HashMap<&str, Vec<usize>> = HashMap::new();
    for (i, nd) in g.nodes.iter().enumerate() {
        for o in &nd.outs {
            producers.entry(o.as_str()).or_default().push(i);
        }
    }
    let producers: HashMap<String, Vec<usize>> = producers
        .into_iter()
        .map(|(k, v)| (k.to_string(), v))
        .collect();
    for i in 0..n {
        let dyns = g.nodes[i].dyns.clone();
        let keep = g.nodes[i].ndeps_static;
        g.nodes[i].deps.truncate(keep);
        for d in dyns {
            if let Some(js) = producers.get(d.as_str()) {
                for &j in js {
                    if j == i {
                        continue;
                    }
                    node_add_dep(&mut g.nodes[i], j);
                }
            }
        }
    }
    for i in 0..n {
        g.nodes[i].rdeps.clear();
        g.nodes[i].indeg = g.nodes[i].deps.len();
    }
    build_rdeps(g)?;
    detect_cycle(g)
}

pub fn parse(text: &str) -> Result<Graph, String> {
    let mut nodes: Vec<Node> = Vec::new();
    let mut lineno = 0usize;
    let lines: Vec<&str> = text.split('\n').collect();
    for raw in lines {
        lineno += 1;
        let s = raw.trim_matches(|c: char| c.is_ascii_whitespace() || c == '\u{b}');
        if s.is_empty() || s.starts_with('#') {
            continue;
        }
        let colon = match s.find(':') {
            Some(i) => i,
            None => return Err(format!("line {}: missing ':'", lineno)),
        };
        let (head, rest) = s.split_at(colon);
        let rest = &rest[1..];
        let declared = atoi(head);
        let node_idx = nodes.len();
        if declared != node_idx as i64 {
            return Err(format!(
                "line {}: expected index {}, got {}",
                lineno, node_idx, declared
            ));
        }

        let mut rest = rest;
        let mut deps_str: Option<&str> = None;
        if let Some(i) = find_unquoted(rest, b'<') {
            deps_str = Some(&rest[i + 1..]);
            rest = &rest[..i];
        }
        let mut ins_str: Option<&str> = None;
        if let Some(i) = find_unquoted(rest, b'@') {
            ins_str = Some(&rest[i + 1..]);
            rest = &rest[..i];
        }
        let mut outs_str: Option<&str> = None;
        if let Some(i) = find_unquoted(rest, b'>') {
            outs_str = Some(&rest[i + 1..]);
            rest = &rest[..i];
        }

        let argv = split_ws(rest);
        if argv.is_empty() {
            return Err(format!("line {}: empty command", lineno));
        }

        let mut nd = Node {
            argv,
            ..Node::default()
        };
        if let Some(ds) = deps_str {
            for t in split_ws(ds) {
                let d = atoi(&t);
                if d < 0 || d >= node_idx as i64 {
                    return Err(format!("line {}: bad dependency {}", lineno, d));
                }
                nd.deps.push(d as usize);
            }
        }
        nd.ndeps_static = nd.deps.len();
        node_scan(&mut nd);
        if let Some(x) = ins_str {
            let mut ins = nd.ins.clone();
            list_add_words(&mut ins, x, false, &nd);
            nd.ins = ins;
        }
        if let Some(x) = outs_str {
            let mut outs = nd.outs.clone();
            list_add_words(&mut outs, x, true, &nd);
            nd.outs = outs;
        }
        nodes.push(nd);
    }

    let mut g = Graph { nodes };
    for i in 0..g.nodes.len() {
        g.nodes[i].indeg = g.nodes[i].deps.len();
    }
    build_rdeps(&mut g)?;
    detect_cycle(&g)?;
    Ok(g)
}

pub fn add_dyn(nd: &mut Node, path: &str) {
    if !has_dyn(nd, path) {
        nd.dyns.push(path.to_string());
    }
}

pub fn has_dyn(nd: &Node, path: &str) -> bool {
    nd.dyns.iter().any(|d| d == path)
}

pub fn clear_dyn(nd: &mut Node) {
    nd.dyns.clear();
}

#[allow(dead_code)]
pub fn join_argv(argv: &[String]) -> String {
    let mut out = String::new();
    for (i, a) in argv.iter().enumerate() {
        if i > 0 {
            out.push(' ');
        }
        out.push_str(a);
    }
    out
}
