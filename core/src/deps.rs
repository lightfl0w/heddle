use crate::graph::{self, Graph, Node};
use crate::hash::{hash_read_file, hash_u64, HashDb};
use crate::sys;
use std::collections::HashSet;

fn resolve_include(nd: &Node, from_dir: &str, name: &str, angled: bool) -> Option<String> {
    if !angled {
        let cand = sys::join(from_dir, name);
        if sys::stat(&cand).is_some() {
            return Some(cand);
        }
    }
    for d in &nd.incdirs {
        let cand = sys::join(d, name);
        if sys::stat(&cand).is_some() {
            return Some(cand);
        }
    }
    let cand = sys::join(".", name);
    if sys::stat(&cand).is_some() {
        return Some(cand);
    }
    None
}

fn scan_file(nd: &Node, path: &str, visited: &mut HashSet<String>, out: &mut Vec<String>) {
    if !visited.insert(path.to_string()) {
        return;
    }
    let text = match sys::read_to_string(path) {
        Some(t) => t,
        None => return,
    };
    let dir = sys::dir_of(path);
    for raw in text.lines() {
        let s = raw.trim();
        let first = match s.chars().next() {
            Some(c) => c,
            None => continue,
        };
        if first != '#' && first != '%' {
            continue;
        }
        let s = s[first.len_utf8()..].trim();
        if !s.starts_with("include") {
            continue;
        }
        match s[7..].chars().next() {
            Some(c) if c.is_ascii_whitespace() || c == '"' || c == '<' || c == '\'' => {}
            _ => continue,
        }
        let s = s[7..].trim();
        let q = match s.chars().next() {
            Some(c) => c,
            None => continue,
        };
        if q != '"' && q != '<' && q != '\'' {
            continue;
        }
        let close = if q == '<' { '>' } else { q };
        let rest = &s[q.len_utf8()..];
        let end = match rest.find(close) {
            Some(i) => i,
            None => continue,
        };
        let name = &rest[..end];
        let angled = q == '<';
        if let Some(resolved) = resolve_include(nd, &dir, name, angled) {
            if !out.contains(&resolved) {
                out.push(resolved.clone());
            }
            scan_file(nd, &resolved, visited, out);
        }
    }
}

fn input_key(nd: &Node, files: &mut HashDb) -> u64 {
    let mut k = nd.cmd_hash;
    for i in &nd.ins {
        k = hash_u64(k, hash_read_file(Some(files), i));
    }
    k
}

fn scan_node(g: &mut Graph, node: usize, files: &mut HashDb) {
    let nd = &g.nodes[node];
    let ins = nd.ins.clone();
    let mut visited: HashSet<String> = HashSet::new();
    let mut out: Vec<String> = Vec::new();
    for i in &ins {
        scan_file(nd, i, &mut visited, &mut out);
    }
    let _ = files;
    let nd = &mut g.nodes[node];
    nd.dyns.clear();
    for p in out {
        graph::add_dyn(nd, &p);
    }
}

pub fn scan(g: &mut Graph, files: &mut HashDb) {
    for i in 0..g.nodes.len() {
        let key = input_key(&g.nodes[i], files);
        if key == g.nodes[i].scan_hash {
            continue;
        }
        scan_node(g, i, files);
        g.nodes[i].scan_hash = key;
    }
}

pub fn load(g: &mut Graph, path: &str) {
    let text = match sys::read_to_string(path) {
        Some(t) => t,
        None => return,
    };
    for line in text.lines() {
        let mut it = line.splitn(4, char::is_whitespace);
        let tag = match it.next() {
            Some(t) => t,
            None => continue,
        };
        if tag != "D" {
            continue;
        }
        let node_s = match it.next() {
            Some(v) => v,
            None => continue,
        };
        let key_s = match it.next() {
            Some(v) => v,
            None => continue,
        };
        let rest = it.next().unwrap_or("");
        let node: i64 = node_s.parse().unwrap_or(-1);
        if node < 0 || node as usize >= g.nodes.len() {
            continue;
        }
        let key = u64::from_str_radix(key_s, 16).unwrap_or(0);
        let nd = &mut g.nodes[node as usize];
        nd.dyns.clear();
        for t in rest.split_whitespace() {
            graph::add_dyn(nd, t);
        }
        nd.scan_hash = key;
    }
}

pub fn save(g: &Graph, path: &str) -> bool {
    let mut out = String::new();
    for (i, nd) in g.nodes.iter().enumerate() {
        out.push_str(&format!("D {} {:x}", i, nd.scan_hash));
        for d in &nd.dyns {
            out.push(' ');
            out.push_str(d);
        }
        out.push('\n');
    }
    sys::write_bytes(path, out.as_bytes())
}
