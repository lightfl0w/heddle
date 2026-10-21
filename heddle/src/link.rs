use crate::model::Toolchain;

#[derive(Default)]
pub struct LinkReq {
    pub out: String,
    pub objs: Vec<String>,
    pub libs: Vec<String>,
    pub ldflags: Vec<String>,
    pub ldscript: Option<String>,
    pub entry: Option<String>,
    pub shared: bool,
    pub rsp: Option<String>,
}

fn put_inputs(s: &mut String, r: &LinkReq) {
    if let Some(rsp) = &r.rsp {
        s.push_str(&format!(" \"@{}\"", rsp));
        return;
    }
    for o in &r.objs {
        s.push(' ');
        s.push_str(o);
    }
    for l in &r.libs {
        s.push(' ');
        s.push_str(l);
    }
}

fn put_flags(s: &mut String, r: &LinkReq) {
    if r.rsp.is_some() {
        return;
    }
    for f in &r.ldflags {
        s.push(' ');
        s.push_str(f);
    }
}

fn msvc_lib_ok(f: &str) -> bool {
    if f == "-nostdlib" || f == "-Wl,--build-id=none" {
        return false;
    }
    !f.starts_with('-')
}

fn put_msvc_ldflags(s: &mut String, r: &LinkReq) {
    if r.rsp.is_some() {
        return;
    }
    for f in &r.ldflags {
        if f.is_empty() {
            continue;
        }
        if let Some(rest) = f.strip_prefix("-Wl,") {
            s.push(' ');
            s.push_str(rest);
        } else if msvc_lib_ok(f) {
            s.push(' ');
            s.push_str(f);
        }
    }
}

fn gnu_cmd(tc: &Toolchain, r: &LinkReq) -> String {
    let mut s = String::new();
    s.push_str(&tc.ld);
    if r.shared && !tc.soflag.is_empty() {
        s.push(' ');
        s.push_str(&tc.soflag);
    }
    s.push_str(&format!(" -o {}", r.out));
    put_inputs(&mut s, r);
    if let Some(e) = &r.entry {
        s.push_str(&format!(" -e {}", e));
    }
    if let Some(l) = &r.ldscript {
        s.push_str(&format!(" -T {}", l));
    }
    put_flags(&mut s, r);
    s
}

fn armcc_cmd(tc: &Toolchain, r: &LinkReq) -> String {
    let mut s = format!("{} --output={}", tc.ld, r.out);
    if r.shared {
        s.push_str(" --shared");
    }
    if let Some(l) = &r.ldscript {
        s.push_str(&format!(" --scatter={}", l));
    }
    if let Some(e) = &r.entry {
        s.push_str(&format!(" --entry={}", e));
    }
    put_inputs(&mut s, r);
    put_flags(&mut s, r);
    s
}

fn iar_cmd(tc: &Toolchain, r: &LinkReq) -> String {
    let mut s = format!("{} --output={}", tc.ld, r.out);
    if r.shared {
        s.push_str(" --dlib_config shared");
    }
    if let Some(l) = &r.ldscript {
        s.push_str(&format!(" --config={}", l));
    }
    if let Some(e) = &r.entry {
        s.push_str(&format!(" --entry={}", e));
    }
    put_inputs(&mut s, r);
    put_flags(&mut s, r);
    s
}

fn msvc_cmd(tc: &Toolchain, r: &LinkReq) -> String {
    let mut s = format!("{} /nologo", tc.ld);
    if r.shared {
        s.push_str(" /LD");
    }
    put_inputs(&mut s, r);
    if let Some(l) = &r.ldscript {
        s.push_str(&format!(" /DEF:{}", l));
    }
    if let Some(e) = &r.entry {
        s.push_str(&format!(" /ENTRY:{}", e));
    }
    s.push_str(&format!(" /Fe{} /link", r.out));
    put_msvc_ldflags(&mut s, r);
    s
}

pub fn link_cmd(tc: &Toolchain, r: &LinkReq) -> String {
    match tc.family.as_str() {
        "armcc" => armcc_cmd(tc, r),
        "iar" => iar_cmd(tc, r),
        "msvc" => msvc_cmd(tc, r),
        _ => gnu_cmd(tc, r),
    }
}

pub fn link_cmd_len(tc: &Toolchain, r: &LinkReq) -> usize {
    link_cmd(tc, r).len()
}
