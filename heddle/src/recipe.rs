use crate::glob;
use crate::model::Recipe;
use crate::sys;
use crate::toml::Toml;

fn exists(p: &str) -> bool {
    sys::stat(p).is_some()
}

fn load_list(t: &Toml, sect: &str, key: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut i = 0usize;
    while let Some(v) = t.arr(sect, key, i) {
        out.push(v.to_string());
        i += 1;
    }
    out
}

pub fn match_dir(dir: &str) -> bool {
    exists(&sys::path_join(dir, "package.toml"))
}

pub fn load(dir: &str) -> Result<Recipe, String> {
    let mut r = Recipe {
        dir: dir.to_string(),
        ..Recipe::default()
    };

    let cfg = sys::path_join(dir, "package.toml");

    if !exists(&cfg) {
        return Err(format!("cannot read {}", cfg));
    }

    let t = Toml::parse(&cfg)?;

    if let Some(v) = t.str_of("package", "name") {
        r.pkg.name = v.to_string();
    }
    if let Some(v) = t.str_of("package", "version") {
        r.pkg.version = v.to_string();
    }
    if let Some(v) = t.str_of("source", "url") {
        r.source.url = v.to_string();
    }
    if let Some(v) = t.str_of("source", "tag") {
        r.source.tag = v.to_string();
    }
    if let Some(v) = t.str_of("source", "path") {
        r.source.path = v.to_string();
    }
    if let Some(v) = t.str_of("build", "type") {
        r.build.kind = Some(v.to_string());
    }

    r.build.patterns = load_list(&t, "build", "sources");
    r.build.include_dirs = load_list(&t, "build", "include_dirs");
    r.build.defines = load_list(&t, "build", "defines");
    r.build.cflags = load_list(&t, "build", "cflags");

    if r.pkg.name.is_empty() {
        return Err(format!("{}: missing [package] name", dir));
    }

    for pat in r.build.patterns.clone() {
        for f in glob::glob_dir(&r.dir, &pat) {
            r.build.files.push(sys::path_join(&r.dir, &f));
        }
    }

    if !r.build.patterns.is_empty() && r.build.files.is_empty() {
        return Err(format!("{}: no sources match", dir));
    }

    Ok(r)
}
