use crate::json::Json;
use crate::sys;
use loom_core::proc::run;

pub struct MigTarget {
    pub name: String,
    pub kind: String,
    pub src: Vec<String>,
    pub inc: Vec<String>,
    pub deps: Vec<String>,
    pub cflags: Vec<String>,
    pub ldflags: Vec<String>,
    pub ldscript: Option<String>,
}

impl MigTarget {
    fn new(name: &str) -> MigTarget {
        MigTarget {
            name: name.to_string(),
            kind: "exe".to_string(),
            src: Vec::new(),
            inc: Vec::new(),
            deps: Vec::new(),
            cflags: Vec::new(),
            ldflags: Vec::new(),
            ldscript: None,
        }
    }
}

#[derive(Default)]
pub struct MigSet {
    pub items: Vec<MigTarget>,
    pub warn: Vec<String>,
}

fn is_space(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

fn target_get(items: &[MigTarget], name: &str) -> Option<usize> {
    items.iter().position(|t| t.name == name)
}

fn target_add(s: &mut MigSet, name: &str) -> usize {
    if let Some(i) = target_get(&s.items, name) {
        return i;
    }
    s.items.push(MigTarget::new(name));
    s.items.len() - 1
}

fn warn(s: &mut MigSet, msg: String) {
    s.warn.push(msg);
}

fn push_uniq(v: &mut Vec<String>, s: &str) {
    if !v.iter().any(|x| x == s) {
        v.push(s.to_string());
    }
}

fn is_abs(p: &str) -> bool {
    if p.starts_with('/') {
        return true;
    }
    #[cfg(windows)]
    {
        let b = p.as_bytes();
        if b.len() >= 2 && b[0].is_ascii_alphabetic() && b[1] == b':' {
            return true;
        }
        if p.starts_with('\\') {
            return true;
        }
    }
    false
}

fn join_from(dir: &str, rel: &str) -> String {
    if is_abs(rel) {
        return rel.to_string();
    }
    format!("{}/{}", dir, rel)
}

fn strip_comments(b: &mut [u8]) {
    let mut q = false;
    let mut line = false;
    let mut i = 0usize;
    while i < b.len() {
        let c = b[i];
        if c == b'\n' {
            line = false;
            i += 1;
            continue;
        }
        if q {
            if c == b'"' {
                q = false;
            }
            i += 1;
            continue;
        }
        if c == b'"' {
            q = true;
            i += 1;
            continue;
        }
        if !line && c == b'#' {
            let mut e = i;
            while e < b.len() && b[e] != b'\n' {
                b[e] = b' ';
                e += 1;
            }
            line = true;
        }
        i += 1;
    }
}

fn split_args_ex(s: &[u8], commas: bool) -> Vec<String> {
    let mut out = Vec::new();
    let mut p = 0usize;
    while p < s.len() {
        while p < s.len() && (is_space(s[p]) || (commas && s[p] == b',')) {
            p += 1;
        }
        if p >= s.len() {
            break;
        }
        let mut buf: Vec<u8> = Vec::new();
        let mut q = false;
        loop {
            if p >= s.len() {
                break;
            }
            let stop = if q {
                false
            } else if commas {
                s[p] == b',' || is_space(s[p])
            } else {
                is_space(s[p])
            };
            if stop {
                break;
            }
            if s[p] == b'"' {
                q = !q;
                p += 1;
                continue;
            }
            buf.push(s[p]);
            p += 1;
        }
        if !buf.is_empty() {
            out.push(String::from_utf8_lossy(&buf).into_owned());
        }
    }
    out
}

fn is_scope(s: &str) -> bool {
    s.eq_ignore_ascii_case("PUBLIC")
        || s.eq_ignore_ascii_case("PRIVATE")
        || s.eq_ignore_ascii_case("INTERFACE")
        || s.eq_ignore_ascii_case("SYSTEM")
}

fn kind_of_cmake(kw: &str) -> &'static str {
    if kw.eq_ignore_ascii_case("STATIC") {
        return "staticlib";
    }
    if kw.eq_ignore_ascii_case("SHARED") {
        return "sharedlib";
    }
    if kw.eq_ignore_ascii_case("MODULE") {
        return "sharedlib";
    }
    "exe"
}

fn xmake_kind(k: &str) -> &'static str {
    if k.eq_ignore_ascii_case("static") {
        return "staticlib";
    }
    if k.eq_ignore_ascii_case("shared") {
        return "sharedlib";
    }
    if k.eq_ignore_ascii_case("binary") {
        return "exe";
    }
    "exe"
}

fn mig_resolve(s: &mut MigSet) {
    for i in 0..s.items.len() {
        let mut k = 0usize;
        while k < s.items[i].deps.len() {
            let d = s.items[i].deps[k].clone();
            if target_get(&s.items, &d).is_some() {
                k += 1;
                continue;
            }
            let l = format!("-l{}", d);
            s.items[i].ldflags.push(l);
            s.items[i].deps.remove(k);
        }
    }
}

fn cmake_command(s: &mut MigSet, name: &str, args: &[String]) {
    if name.eq_ignore_ascii_case("add_executable") && !args.is_empty() {
        let ti = target_add(s, &args[0]);
        s.items[ti].kind = "exe".to_string();
        for a in &args[1..] {
            s.items[ti].src.push(a.clone());
        }
    } else if name.eq_ignore_ascii_case("add_library") && args.len() >= 2 {
        let ti = target_add(s, &args[0]);
        s.items[ti].kind = kind_of_cmake(&args[1]).to_string();
        for a in &args[2..] {
            s.items[ti].src.push(a.clone());
        }
    } else if name.eq_ignore_ascii_case("target_sources") && args.len() >= 2 {
        if let Some(ti) = target_get(&s.items, &args[0]) {
            for a in &args[1..] {
                if !is_scope(a) {
                    s.items[ti].src.push(a.clone());
                }
            }
        }
    } else if name.eq_ignore_ascii_case("target_include_directories") && args.len() >= 2 {
        if let Some(ti) = target_get(&s.items, &args[0]) {
            for a in &args[1..] {
                if !is_scope(a) {
                    s.items[ti].inc.push(a.clone());
                }
            }
        }
    } else if name.eq_ignore_ascii_case("target_link_libraries") && args.len() >= 2 {
        if let Some(ti) = target_get(&s.items, &args[0]) {
            for a in &args[1..] {
                if is_scope(a) {
                    continue;
                }
                if a.starts_with('-') || a.contains(':') {
                    s.items[ti].ldflags.push(a.clone());
                } else {
                    s.items[ti].deps.push(a.clone());
                }
            }
        }
    } else if name.eq_ignore_ascii_case("target_compile_definitions") && args.len() >= 2 {
        if let Some(ti) = target_get(&s.items, &args[0]) {
            for a in &args[1..] {
                if is_scope(a) {
                    continue;
                }
                s.items[ti].cflags.push(format!("-D{}", a));
            }
        }
    } else if name.eq_ignore_ascii_case("target_compile_options") && args.len() >= 2 {
        if let Some(ti) = target_get(&s.items, &args[0]) {
            for a in &args[1..] {
                if !is_scope(a) {
                    s.items[ti].cflags.push(a.clone());
                }
            }
        }
    } else if name.eq_ignore_ascii_case("target_link_options") && args.len() >= 2 {
        if let Some(ti) = target_get(&s.items, &args[0]) {
            let mut i = 1usize;
            while i < args.len() {
                let a = &args[i];
                if is_scope(a) {
                    i += 1;
                    continue;
                }
                if a == "-T" && i + 1 < args.len() {
                    s.items[ti].ldscript = Some(args[i + 1].clone());
                    i += 2;
                    continue;
                }
                if a.starts_with("-T") && a.len() > 2 {
                    s.items[ti].ldscript = Some(a[2..].to_string());
                    i += 1;
                    continue;
                }
                if a.len() > 4 && a.starts_with("-Wl,") && a[4..].starts_with("-T,") {
                    s.items[ti].ldscript = Some(a[7..].to_string());
                    i += 1;
                    continue;
                }
                s.items[ti].ldflags.push(a.clone());
                i += 1;
            }
        }
    } else if name.eq_ignore_ascii_case("add_definitions") {
        for i in 0..s.items.len() {
            for a in args {
                s.items[i].cflags.push(a.clone());
            }
        }
    } else if name.eq_ignore_ascii_case("set")
        && args.len() >= 3
        && args[1].eq_ignore_ascii_case("LINKER_LANGUAGE")
    {
        warn(s, format!("ignored: set({})", args[0]));
    } else if name.eq_ignore_ascii_case("include_directories")
        || name.eq_ignore_ascii_case("add_compile_options")
    {
        warn(
            s,
            format!("global '{}' not translated, add it to targets by hand", name),
        );
    }
}

fn migrate_load_cmake(s: &mut MigSet, path: &str) -> Result<(), String> {
    let text = match sys::read_to_string(path) {
        Some(t) => t,
        None => return Err(format!("cannot read {}", path)),
    };
    let mut b = text.into_bytes();
    strip_comments(&mut b);
    let n = b.len();
    let mut p = 0usize;
    while p < n {
        while p < n && (is_space(b[p]) || b[p] == b')') {
            p += 1;
        }
        if p >= n {
            break;
        }
        let nb = p;
        while p < n && (b[p].is_ascii_alphanumeric() || b[p] == b'_') {
            p += 1;
        }
        if p == nb {
            p += 1;
            continue;
        }
        let name = String::from_utf8_lossy(&b[nb..p]).into_owned();
        if name.len() >= 128 {
            p += 1;
            continue;
        }
        while p < n && is_space(b[p]) {
            p += 1;
        }
        if p >= n || b[p] != b'(' {
            continue;
        }
        p += 1;
        let ab = p;
        let mut depth = 1i32;
        while p < n && depth > 0 {
            if b[p] == b'(' {
                depth += 1;
            } else if b[p] == b')' {
                depth -= 1;
            }
            if depth > 0 {
                p += 1;
            }
        }
        let args = split_args_ex(&b[ab..p], false);
        if p < n && b[p] == b')' {
            p += 1;
        }
        cmake_command(s, &name, &args);
    }
    if s.items.is_empty() {
        return Err(format!("{}: no add_executable/add_library found", path));
    }
    mig_resolve(s);
    Ok(())
}

fn c_lines(text: &[u8]) -> Vec<&[u8]> {
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < text.len() {
        let mut n = 0usize;
        while i + n < text.len() && n < 8191 {
            let c = text[i + n];
            n += 1;
            if c == b'\n' {
                break;
            }
        }
        out.push(&text[i..i + n]);
        i += n;
    }
    out
}

fn lua_strip(line: &mut [u8]) {
    if line.len() < 2 {
        return;
    }
    let mut hit = None;
    let mut i = 0usize;
    while i + 1 < line.len() {
        if line[i] == b'-' && line[i + 1] == b'-' {
            hit = Some(i);
            break;
        }
        i += 1;
    }
    if let Some(p) = hit {
        let mut e = p;
        while e < line.len() && line[e] != b'\n' {
            line[e] = b' ';
            e += 1;
        }
    }
}

fn lua_fn(line: &[u8]) -> Option<String> {
    let mut p = 0usize;
    while p < line.len() && is_space(line[p]) {
        p += 1;
    }
    let b = p;
    while p < line.len() && (line[p].is_ascii_alphanumeric() || line[p] == b'_') {
        p += 1;
    }
    let n = p - b;
    if n == 0 || n >= 64 {
        return None;
    }
    while p < line.len() && is_space(line[p]) {
        p += 1;
    }
    if p >= line.len() || line[p] != b'(' {
        return None;
    }
    Some(String::from_utf8_lossy(&line[b..b + n]).into_owned())
}

fn lua_args(line: &[u8]) -> Vec<String> {
    let lp = match line.iter().position(|&c| c == b'(') {
        Some(i) => i,
        None => return Vec::new(),
    };
    let rp = match line[lp..].iter().rposition(|&c| c == b')') {
        Some(i) => lp + i,
        None => return Vec::new(),
    };
    split_args_ex(&line[lp + 1..rp], true)
}

fn migrate_load_xmake(s: &mut MigSet, path: &str) -> Result<(), String> {
    let text = match sys::read_to_string(path) {
        Some(t) => t,
        None => return Err(format!("cannot read {}", path)),
    };
    let mut cur: Option<usize> = None;
    for raw in c_lines(text.as_bytes()) {
        let mut line = raw.to_vec();
        lua_strip(&mut line);
        let fname = match lua_fn(&line) {
            Some(f) => f,
            None => continue,
        };
        let args = lua_args(&line);
        if fname.eq_ignore_ascii_case("target") && !args.is_empty() {
            cur = Some(target_add(s, &args[0]));
            continue;
        }
        let ti = match cur {
            Some(t) => t,
            None => continue,
        };
        if fname.eq_ignore_ascii_case("set_kind") && !args.is_empty() {
            s.items[ti].kind = xmake_kind(&args[0]).to_string();
        } else if fname.eq_ignore_ascii_case("add_files")
            || fname.eq_ignore_ascii_case("add_headerfiles")
        {
            for a in &args {
                s.items[ti].src.push(a.clone());
            }
        } else if fname.eq_ignore_ascii_case("remove_files") {
            for a in &args {
                let mut hit = false;
                let mut k = 0usize;
                while k < s.items[ti].src.len() {
                    if s.items[ti].src[k].as_str() != a.as_str() {
                        k += 1;
                        continue;
                    }
                    s.items[ti].src.remove(k);
                    hit = true;
                    break;
                }
                if !hit {
                    warn(s, format!("remove_files({}) not matched, prune by hand", a));
                }
            }
        } else if fname.eq_ignore_ascii_case("add_includedirs") {
            for a in &args {
                s.items[ti].inc.push(a.clone());
            }
        } else if fname.eq_ignore_ascii_case("add_deps") || fname.eq_ignore_ascii_case("add_links") {
            for a in &args {
                s.items[ti].deps.push(a.clone());
            }
        } else if fname.eq_ignore_ascii_case("add_defines") {
            for a in &args {
                s.items[ti].cflags.push(format!("-D{}", a));
            }
        } else if fname.eq_ignore_ascii_case("add_cxflags")
            || fname.eq_ignore_ascii_case("add_cflags")
            || fname.eq_ignore_ascii_case("add_cxxflags")
        {
            for a in &args {
                s.items[ti].cflags.push(a.clone());
            }
        } else if fname.eq_ignore_ascii_case("add_ldflags")
            || fname.eq_ignore_ascii_case("add_ldscripts")
        {
            let mut i = 0usize;
            while i < args.len() {
                if args[i] == "-T" && i + 1 < args.len() {
                    s.items[ti].ldscript = Some(args[i + 1].clone());
                    i += 2;
                    continue;
                }
                if args[i].starts_with("-T") && args[i].len() > 2 {
                    s.items[ti].ldscript = Some(args[i][2..].to_string());
                    i += 1;
                    continue;
                }
                s.items[ti].ldflags.push(args[i].clone());
                i += 1;
            }
        } else if fname.eq_ignore_ascii_case("add_syslinks") {
            for a in &args {
                s.items[ti].ldflags.push(format!("-l{}", a));
            }
        }
    }
    if s.items.is_empty() {
        return Err(format!("{}: no target() found", path));
    }
    mig_resolve(s);
    Ok(())
}

fn emit_qstr(out: &mut Vec<u8>, s: &str) {
    out.push(b'"');
    for &c in s.as_bytes() {
        if c == b'"' || c == b'\\' {
            out.push(b'\\');
        }
        out.push(c);
    }
    out.push(b'"');
}

fn emit_list(out: &mut Vec<u8>, key: &str, v: &[String]) {
    if v.is_empty() {
        return;
    }
    out.extend_from_slice(format!("{} = [", key).as_bytes());
    for (i, x) in v.iter().enumerate() {
        if i > 0 {
            out.extend_from_slice(b", ");
        }
        emit_qstr(out, x);
    }
    out.extend_from_slice(b"]\n");
}

fn star_list(out: &mut Vec<u8>, key: &str, v: &[String]) {
    if v.is_empty() {
        return;
    }
    out.extend_from_slice(format!("    {} = [", key).as_bytes());
    for (i, x) in v.iter().enumerate() {
        if i > 0 {
            out.extend_from_slice(b", ");
        }
        emit_qstr(out, x);
    }
    out.extend_from_slice(b"],\n");
}

fn migrate_write(s: &MigSet, out: &str) -> Result<(), String> {
    let mut b: Vec<u8> = Vec::new();
    b.extend_from_slice(b"[build]\ndir = \"out\"\n");
    for t in &s.items {
        b.extend_from_slice(format!("\n[target.{}]\ntype = \"{}\"\n", t.name, t.kind).as_bytes());
        emit_list(&mut b, "src", &t.src);
        emit_list(&mut b, "inc", &t.inc);
        emit_list(&mut b, "deps", &t.deps);
        emit_list(&mut b, "cflags", &t.cflags);
        emit_list(&mut b, "ldflags", &t.ldflags);
        if let Some(ld) = &t.ldscript {
            b.extend_from_slice(b"linker_script = ");
            emit_qstr(&mut b, ld);
            b.push(b'\n');
        }
    }
    if !sys::write_bytes(out, &b) {
        return Err(format!("cannot write {}", out));
    }
    Ok(())
}

fn clip200(s: &str) -> String {
    if s.len() <= 200 {
        return s.to_string();
    }
    String::from_utf8_lossy(&s.as_bytes()[..200]).into_owned()
}

fn migrate_write_star(s: &MigSet, out: &str) -> Result<(), String> {
    let mut b: Vec<u8> = Vec::new();
    b.extend_from_slice(b"build(dir = \"out\")\n");
    for t in &s.items {
        b.extend_from_slice(b"\ntarget(\n    name = ");
        emit_qstr(&mut b, &t.name);
        b.extend_from_slice(b",\n    type = ");
        emit_qstr(&mut b, &t.kind);
        b.extend_from_slice(b",\n");
        star_list(&mut b, "src", &t.src);
        star_list(&mut b, "inc", &t.inc);
        star_list(&mut b, "deps", &t.deps);
        star_list(&mut b, "cflags", &t.cflags);
        star_list(&mut b, "ldflags", &t.ldflags);
        if let Some(ld) = &t.ldscript {
            b.extend_from_slice(b"    linker_script = ");
            emit_qstr(&mut b, ld);
            b.extend_from_slice(b",\n");
        }
        b.extend_from_slice(b")\n");
    }
    if !sys::write_bytes(out, &b) {
        return Err(format!("cannot write {}", clip200(out)));
    }
    Ok(())
}

fn migrate_print(s: &MigSet) {
    println!(
        "heddle: {} target{}",
        s.items.len(),
        if s.items.len() == 1 { "" } else { "s" }
    );
    for t in &s.items {
        println!(
            "  {:<16} {:<10} src={} inc={} deps={}",
            t.name,
            t.kind,
            t.src.len(),
            t.inc.len(),
            t.deps.len()
        );
    }
    if !s.warn.is_empty() {
        println!(
            "heddle: {} note{}",
            s.warn.len(),
            if s.warn.len() == 1 { "" } else { "s" }
        );
        for w in &s.warn {
            println!("  - {}", w);
        }
    }
}

fn json_key<'a>(j: &'a Json, base: &str, key: &str) -> Option<&'a str> {
    if base.is_empty() {
        j.get(key)
    } else {
        j.get(&format!("{}.{}", base, key))
    }
}

fn has_node(j: &Json, path: &str) -> bool {
    if j.get(path).is_some() {
        return true;
    }
    let dot = format!("{}.", path);
    let br = format!("{}[", path);
    j.items
        .iter()
        .any(|(k, _)| k.starts_with(&dot) || k.starts_with(&br))
}

fn arr_len(j: &Json, path: &str) -> usize {
    let mut n = 0usize;
    while has_node(j, &format!("{}[{}]", path, n)) {
        n += 1;
    }
    n
}

fn map_type(t: Option<&str>) -> &'static str {
    match t {
        Some("STATIC_LIBRARY") | Some("OBJECT_LIBRARY") => "staticlib",
        Some("SHARED_LIBRARY") | Some("MODULE_LIBRARY") => "sharedlib",
        _ => "exe",
    }
}

fn is_internal(nm: &str, ty: Option<&str>) -> bool {
    if ty == Some("UTILITY") {
        return true;
    }
    const KNOWN: [&str; 7] = [
        "ALL_BUILD",
        "ZERO_CHECK",
        "INSTALL",
        "RUN_TESTS",
        "PACKAGE",
        "edit_cache",
        "rebuild_cache",
    ];
    if KNOWN.iter().any(|k| *k == nm) {
        return true;
    }
    nm.starts_with("Nightly") || nm.starts_with("Continuous") || nm.starts_with("Experimental")
}

fn id_to_name(id: &str) -> String {
    match id.find("::") {
        Some(i) => id[..i].to_string(),
        None => id.to_string(),
    }
}

fn add_sources(t: &mut MigTarget, j: &Json, src: &str) {
    let n = arr_len(j, "sources");
    for i in 0..n {
        let base = format!("sources[{}]", i);
        if let Some(p) = json_key(j, &base, "path") {
            t.src.push(join_from(src, p));
        }
    }
}

fn add_compile(t: &mut MigTarget, j: &Json) {
    let ng = arr_len(j, "compileGroups");
    for g in 0..ng {
        let gp = format!("compileGroups[{}]", g);
        let ni = arr_len(j, &format!("{}.includes", gp));
        for i in 0..ni {
            let p = format!("{}.includes[{}]", gp, i);
            if let Some(v) = json_key(j, &p, "path") {
                push_uniq(&mut t.inc, v);
            }
        }
        let nd = arr_len(j, &format!("{}.defines", gp));
        for i in 0..nd {
            let p = format!("{}.defines[{}]", gp, i);
            if let Some(v) = json_key(j, &p, "define") {
                push_uniq(&mut t.cflags, &format!("-D{}", v));
            }
        }
        let nf = arr_len(j, &format!("{}.compileCommandFragments", gp));
        for i in 0..nf {
            let p = format!("{}.compileCommandFragments[{}]", gp, i);
            if let Some(v) = json_key(j, &p, "fragment") {
                if !v.is_empty() && !v.starts_with('/') {
                    push_uniq(&mut t.cflags, v);
                }
            }
        }
    }
}

fn add_link(t: &mut MigTarget, j: &Json) {
    let n = arr_len(j, "link.commandFragments");
    for i in 0..n {
        let p = format!("link.commandFragments[{}]", i);
        let fr = match json_key(j, &p, "fragment") {
            Some(v) if !v.is_empty() => v,
            _ => continue,
        };
        let role = json_key(j, &p, "role");
        if role == Some("libraries") && (fr.contains(".a") || fr.contains(".lib")) {
            continue;
        }
        push_uniq(&mut t.ldflags, fr);
    }
}

fn add_deps(s: &mut MigSet, ti: usize, j: &Json) {
    for key in ["linkLibraries", "dependencies"] {
        let n = arr_len(j, key);
        for i in 0..n {
            let p = format!("{}[{}]", key, i);
            let id = json_key(j, &p, "id");
            let frg = json_key(j, &p, "fragment");
            let mut used_id = false;
            if let Some(v) = id {
                if !v.is_empty() {
                    used_id = true;
                    let name = id_to_name(v);
                    if !name.is_empty() && target_get(&s.items, &name).is_some() {
                        push_uniq(&mut s.items[ti].deps, &name);
                    }
                }
            }
            if used_id {
                continue;
            }
            if let Some(f) = frg {
                if f.is_empty() {
                    continue;
                }
                if f.starts_with('-') {
                    push_uniq(&mut s.items[ti].ldflags, f);
                } else {
                    let b = format!("-l{}", f);
                    push_uniq(&mut s.items[ti].ldflags, &b);
                }
            }
        }
    }
}

fn grab_ldscript(t: &mut MigTarget) {
    let mut i = 0usize;
    while i < t.ldflags.len() {
        let f = t.ldflags[i].clone();
        let mut drop = 1usize;
        let val = if f == "-T" && i + 1 < t.ldflags.len() {
            drop = 2;
            Some(t.ldflags[i + 1].clone())
        } else if f.starts_with("-T") && f.len() > 2 {
            Some(f[2..].to_string())
        } else {
            None
        };
        match val {
            None => i += 1,
            Some(v) => {
                t.ldscript = Some(v);
                t.ldflags.drain(i..i + drop);
            }
        }
    }
}

#[cfg(windows)]
fn on_path_gcc() -> bool {
    let path = match std::env::var("PATH") {
        Ok(p) => p,
        Err(_) => return false,
    };
    let sep = if path.starts_with('/') { ':' } else { ';' };
    for dir in path.split(sep) {
        if dir.is_empty() {
            continue;
        }
        if sys::stat(&format!("{}/gcc.exe", dir)).is_some() {
            return true;
        }
    }
    false
}

fn find_reply(dir: &str, prefix: &str) -> Option<String> {
    let rd = std::fs::read_dir(dir).ok()?;
    let mut best: Option<String> = None;
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if !name.starts_with(prefix) || name.len() < 5 || !name.ends_with(".json") {
            continue;
        }
        if best.as_ref().map(|b| name > *b).unwrap_or(true) {
            best = Some(name);
        }
    }
    best.map(|n| join_from(dir, &n))
}

fn cmake_api_load(
    s: &mut MigSet,
    srcdir: &str,
    builddir_hint: Option<&str>,
    config_hint: Option<&str>,
    cargs: &[String],
) -> Result<(), String> {
    let builddir = match builddir_hint {
        Some(b) if !b.is_empty() => b.to_string(),
        _ => format!("{}/build-heddle-cmake", srcdir),
    };
    let qdir = format!("{}/.cmake/api/v1/query", builddir);
    sys::mkpath(&qdir);
    for k in ["codemodel-v2", "cache-v2", "toolchains-v1", "cmakeFiles-v1"] {
        sys::write_bytes(&format!("{}/{}", qdir, k), b"");
    }

    let log = format!("{}/heddle-cmake-configure.log", builddir);
    let mut argv: Vec<String> = vec![
        "cmake".to_string(),
        "-S".to_string(),
        srcdir.to_string(),
        "-B".to_string(),
        builddir.clone(),
    ];
    #[cfg(windows)]
    {
        if on_path_gcc() {
            argv.push("-G".to_string());
            argv.push("MSYS Makefiles".to_string());
        }
    }
    for a in cargs {
        if argv.len() >= 63 {
            break;
        }
        argv.push(a.clone());
    }

    let r = match run(&argv, None, &log, &[]) {
        Some(r) => r,
        None => return Err("failed to run cmake (is it on PATH?)".to_string()),
    };
    if r.exit_code != 0 {
        return Err(format!("cmake configure failed (see {})", log));
    }

    let rdir = format!("{}/.cmake/api/v1/reply", builddir);
    let idx = match find_reply(&rdir, "index-").and_then(|p| Json::parse(&p).ok()) {
        Some(j) => j,
        None => {
            return Err(format!(
                "no CMake File API reply in {} (need CMake >= 3.14)",
                rdir
            ))
        }
    };

    let cmf = match json_key(&idx, "reply.codemodel-v2", "jsonFile") {
        Some(v) => v.to_string(),
        None => return Err("no codemodel-v2 reply".to_string()),
    };

    let model = match Json::parse(&join_from(&rdir, &cmf)) {
        Ok(m) => m,
        Err(_) => return Err("cannot parse codemodel-v2".to_string()),
    };

    let ncfg = arr_len(&model, "configurations");
    if ncfg == 0 {
        return Err("codemodel has no configurations".to_string());
    }

    let mut ci = 0usize;
    if let Some(ch) = config_hint {
        if !ch.is_empty() {
            let mut found = false;
            for i in 0..ncfg {
                let base = format!("configurations[{}]", i);
                if let Some(nm) = json_key(&model, &base, "name") {
                    if nm == ch {
                        ci = i;
                        found = true;
                        break;
                    }
                }
            }
            if !found {
                return Err(format!("no configuration '{}'", ch));
            }
        }
    }

    let tbase = format!("configurations[{}].targets", ci);
    let ntg = arr_len(&model, &tbase);
    let mut loaded: Vec<(usize, Json)> = Vec::new();
    for i in 0..ntg {
        let tp = format!("{}[{}]", tbase, i);
        let nm = match json_key(&model, &tp, "name") {
            Some(v) => v.to_string(),
            None => continue,
        };
        let jf = match json_key(&model, &tp, "jsonFile") {
            Some(v) => v.to_string(),
            None => continue,
        };
        let tg = match Json::parse(&join_from(&rdir, &jf)) {
            Ok(t) => t,
            Err(_) => continue,
        };
        let ty = json_key(&tg, "", "type").map(|v| v.to_string());
        if ty.as_deref() == Some("INTERFACE_LIBRARY") || is_internal(&nm, ty.as_deref()) {
            continue;
        }
        let ti = target_add(s, &nm);
        s.items[ti].kind = map_type(ty.as_deref()).to_string();
        add_sources(&mut s.items[ti], &tg, srcdir);
        add_compile(&mut s.items[ti], &tg);
        add_link(&mut s.items[ti], &tg);
        loaded.push((ti, tg));
    }

    for (ti, tg) in &loaded {
        add_deps(s, *ti, tg);
    }

    if s.items.is_empty() {
        return Err("no importable targets".to_string());
    }

    for i in 0..s.items.len() {
        grab_ldscript(&mut s.items[i]);
    }

    let mut dups: Vec<String> = Vec::new();
    for i in 0..s.items.len() {
        for k in 0..s.items[i].src.len() {
            for j in (i + 1)..s.items.len() {
                for m in 0..s.items[j].src.len() {
                    if s.items[i].src[k] == s.items[j].src[m] {
                        dups.push(format!(
                            "source '{}' used by both '{}' and '{}'",
                            s.items[i].src[k], s.items[i].name, s.items[j].name
                        ));
                    }
                }
            }
        }
    }
    for d in dups {
        s.warn.push(d);
    }
    if s.warn.is_empty() {
        warn(s, "imported via CMake File API".to_string());
    }
    Ok(())
}

fn arg_opt(args: &[String], name: &str) -> Option<String> {
    for i in 0..args.len() {
        if args[i] == name && i + 1 < args.len() {
            return Some(args[i + 1].clone());
        }
        if args[i].starts_with(name) {
            let b = args[i].as_bytes();
            if b.get(name.len()) == Some(&b'=') {
                return Some(String::from_utf8_lossy(&b[name.len() + 1..]).into_owned());
            }
        }
    }
    None
}

fn migrate_usage() {
    println!("usage: heddle migrate [DIR] [options]");
    println!();
    println!("  reads CMakeLists.txt or xmake.lua and writes heddle.toml");
    println!("  CMake projects are imported through the CMake File API (a real");
    println!("  configure run), so variables, generator expressions, imported");
    println!("  targets and find_package() results are all resolved.");
    println!();
    println!("options:");
    println!("  --from cmake|xmake   force the source format");
    println!("  --out FILE           output file (default heddle.toml)");
    println!("  --star               write heddle.star instead of heddle.toml");
    println!("  --dry-run            print the plan, write nothing");
    println!("  --cmake-build DIR    build dir used for the File API configure");
    println!("  --config NAME        configuration to import (multi-config gen)");
    println!("  --cmake-arg ARG      extra cmake argument (repeatable)");
    println!("  --no-cmake-api       parse CMakeLists.txt textually instead");
}

pub fn heddle_migrate(args: &[String]) -> i32 {
    let from = arg_opt(args, "--from");
    let out = arg_opt(args, "--out");
    let cmakebuild = arg_opt(args, "--cmake-build");
    let config = arg_opt(args, "--config");
    let mut dry = false;
    let mut star = false;
    let mut no_api = false;
    let mut cargs: Vec<String> = Vec::new();
    let mut dir: Option<String> = None;

    let mut i = 2usize;
    while i < args.len() {
        let a = args[i].as_str();
        if a == "-h" || a == "--help" {
            migrate_usage();
            return 0;
        }
        if a == "--dry-run" {
            dry = true;
            i += 1;
            continue;
        }
        if a == "--star" {
            star = true;
            i += 1;
            continue;
        }
        if a == "--no-cmake-api" {
            no_api = true;
            i += 1;
            continue;
        }
        if a == "--cmake-arg" && i + 1 < args.len() {
            if cargs.len() < 64 {
                cargs.push(args[i + 1].clone());
                i += 2;
            } else {
                i += 1;
            }
            continue;
        }
        if a.starts_with("--cmake-arg=") {
            if cargs.len() < 64 {
                cargs.push(a[12..].to_string());
            }
            i += 1;
            continue;
        }
        if a.starts_with("--") {
            if !a.contains('=') {
                i += 1;
            }
            i += 1;
            continue;
        }
        if a.starts_with('-') {
            i += 1;
            continue;
        }
        if dir.is_none() {
            dir = Some(a.to_string());
        }
        i += 1;
    }

    let dir = match dir {
        Some(d) => d,
        None => ".".to_string(),
    };

    let cmake = format!("{}/CMakeLists.txt", dir);
    let xmake = format!("{}/xmake.lua", dir);
    let mut src: Option<String> = None;
    let mut kind = 0i32;
    match from.as_deref() {
        Some("cmake") => {
            src = Some(cmake.clone());
            kind = 1;
        }
        Some("xmake") => {
            src = Some(xmake.clone());
            kind = 2;
        }
        Some(other) => {
            eprintln!("heddle: unknown --from '{}'", other);
            return 2;
        }
        None => {
            if sys::stat(&cmake).is_some() {
                src = Some(cmake.clone());
                kind = 1;
            } else if sys::stat(&xmake).is_some() {
                src = Some(xmake.clone());
                kind = 2;
            }
        }
    }

    let src = match src {
        Some(s) => s,
        None => {
            eprintln!(
                "heddle: no {}/CMakeLists.txt or {}/xmake.lua",
                dir, dir
            );
            return 1;
        }
    };

    if sys::stat(&src).is_none() {
        eprintln!("heddle: cannot read {}", src);
        return 1;
    }

    let mut s = MigSet::default();
    let rc = if kind == 1 && !no_api {
        match cmake_api_load(
            &mut s,
            &dir,
            cmakebuild.as_deref(),
            config.as_deref(),
            &cargs,
        ) {
            Ok(()) => Ok(()),
            Err(e) => {
                eprintln!("heddle: cmake api import failed: {}", e);
                eprintln!("heddle: falling back to textual CMakeLists.txt");
                s = MigSet::default();
                migrate_load_cmake(&mut s, &src)
            }
        }
    } else if kind == 1 {
        migrate_load_cmake(&mut s, &src)
    } else {
        migrate_load_xmake(&mut s, &src)
    };

    if let Err(e) = rc {
        eprintln!("heddle: {}", e);
        return 1;
    }

    println!("heddle: from {}", src);
    migrate_print(&s);
    if dry {
        return 0;
    }

    let def_out = format!("{}/heddle.{}", dir, if star { "star" } else { "toml" });
    let target_out = match out {
        Some(o) => o,
        None => def_out,
    };
    if sys::stat(&target_out).is_some() {
        eprintln!(
            "heddle: {} exists, use --out to pick another name",
            target_out
        );
        return 1;
    }

    let w = if star {
        migrate_write_star(&s, &target_out)
    } else {
        migrate_write(&s, &target_out)
    };
    if let Err(e) = w {
        eprintln!("heddle: {}", e);
        return 1;
    }

    println!("heddle: wrote {}", target_out);
    0
}
