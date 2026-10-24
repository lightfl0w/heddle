use crate::model::{StarCfg, StarPlatform, StarTargetDef};
use crate::sys;
use rhai::{Dynamic, Engine, Map};
use std::cell::RefCell;
use std::rc::Rc;

type R<T> = Result<T, Box<rhai::EvalAltResult>>;

fn map_str(m: &Map, key: &str) -> Option<String> {
    m.get(key).and_then(|v| v.clone().try_cast::<String>())
}

fn map_list(m: &Map, key: &str) -> Vec<String> {
    m.get(key)
        .and_then(|v| v.clone().try_cast::<rhai::Array>())
        .map(|a| {
            a.into_iter()
                .filter_map(|d| d.try_cast::<String>())
                .collect()
        })
        .unwrap_or_default()
}

fn map_bool(m: &Map, key: &str) -> bool {
    m.get(key)
        .and_then(|v| v.clone().try_cast::<bool>())
        .unwrap_or(false)
}

fn type_name(t: Option<&str>) -> &'static str {
    match t {
        Some("lib") | Some("staticlib") => "staticlib",
        Some("shared") | Some("sharedlib") => "sharedlib",
        Some("raw") => "raw",
        Some("custom") => "custom",
        _ => "exe",
    }
}

fn take_name(m: &Map, f: &str) -> R<String> {
    map_str(m, "name").ok_or_else(|| format!("{}(): missing name", f).into())
}

fn star_dir(glob_pat: &str, root: &str) -> (Option<String>, String) {
    let bytes = glob_pat.as_bytes();
    let wild = bytes.iter().position(|&b| b == b'*' || b == b'?');
    let wild = match wild {
        Some(i) => i,
        None => return (None, glob_pat.to_string()),
    };
    let slash = bytes[..wild].iter().rposition(|&b| b == b'/');
    match slash {
        Some(i) => (
            Some(format!("{}/{}", root, &glob_pat[..i])),
            glob_pat[i + 1..].to_string(),
        ),
        None => (None, glob_pat.to_string()),
    }
}

pub fn run(path: &str) -> Result<StarCfg, String> {
    let bytes = sys::read_bytes(path).ok_or_else(|| format!("cannot read {}", path))?;
    let script = String::from_utf8_lossy(&bytes).into_owned();
    let root = sys::dir_of(path);

    let cfg = Rc::new(RefCell::new(StarCfg::default()));
    let mut engine = Engine::new();

    engine.register_fn("glob", move |pats: rhai::Array| -> R<Dynamic> {
        let mut out: Vec<Dynamic> = Vec::new();
        for pat in pats {
            let pat = match pat.try_cast::<String>() {
                Some(p) => p,
                None => continue,
            };
            let (dir, gp) = star_dir(&pat, &root);
            for m in crate::glob::glob_dir(dir.as_deref().unwrap_or(&root), &gp) {
                match &dir {
                    Some(dd) => out.push(format!("{}/{}", dd, m).into()),
                    None => out.push(m.into()),
                }
            }
        }
        Ok(out.into())
    });

    let c = Rc::clone(&cfg);
    engine.register_fn("build", move |m: Map| {
        let mut cfg = c.borrow_mut();
        cfg.build_dir = map_str(&m, "dir");
        cfg.toolchain = map_str(&m, "toolchain");
        cfg.store = map_str(&m, "store");
    });

    let c = Rc::clone(&cfg);
    engine.register_fn("project", move |m: Map| {
        let mut cfg = c.borrow_mut();
        cfg.project_name = map_str(&m, "name");
        cfg.build_dir = map_str(&m, "build_dir");
        cfg.toolchain = map_str(&m, "default_toolchain");
        cfg.store = map_str(&m, "store");
    });

    let c = Rc::clone(&cfg);
    engine.register_fn("target_config", move |m: Map| {
        let mut cfg = c.borrow_mut();
        cfg.arch = map_str(&m, "arch").unwrap_or_default();
        cfg.abi = map_str(&m, "abi").unwrap_or_default();
        cfg.flt = map_str(&m, "float").unwrap_or_default();
        cfg.sysroot = map_str(&m, "sysroot").unwrap_or_default();
    });

    let c = Rc::clone(&cfg);
    engine.register_fn("toolchain", move |m: Map| -> R<Dynamic> {
        let name = take_name(&m, "toolchain")?;
        let mut cfg = c.borrow_mut();
        cfg.tc_name.push(name.clone());
        cfg.tc_cc.push(map_str(&m, "cc").unwrap_or_default());
        cfg.tc_based
            .push(map_str(&m, "based_on").unwrap_or_default());
        Ok(name.into())
    });

    let c = Rc::clone(&cfg);
    engine.register_fn("pkg", move |m: Map| -> R<Dynamic> {
        let name = take_name(&m, "pkg")?;
        let mut cfg = c.borrow_mut();
        cfg.dep_name.push(name.clone());
        cfg.dep_ver
            .push(map_str(&m, "version").unwrap_or_else(|| "latest".to_string()));
        cfg.dep_src.push(map_str(&m, "source").unwrap_or_default());
        Ok(name.into())
    });

    let c = Rc::clone(&cfg);
    engine.register_fn("platform", move |m: Map| -> R<Dynamic> {
        let name = take_name(&m, "platform")?;
        let p = StarPlatform {
            name: name.clone(),
            arch: map_str(&m, "arch").unwrap_or_default(),
            abi: map_str(&m, "abi").unwrap_or_default(),
            flt: map_str(&m, "float").unwrap_or_default(),
            sysroot: map_str(&m, "sysroot").unwrap_or_default(),
            toolchain: map_str(&m, "toolchain").unwrap_or_default(),
        };
        c.borrow_mut().platforms.push(p);
        Ok(name.into())
    });

    let c = Rc::clone(&cfg);
    engine.register_fn("target", move |m: Map| -> R<Dynamic> {
        let name = take_name(&m, "target")?;
        let mut t = StarTargetDef {
            name,
            kind: Some(
                type_name(map_str(&m, "type").as_deref()).to_string(),
            ),
            src: map_list(&m, "src"),
            inc: map_list(&m, "inc"),
            deps: map_list(&m, "deps"),
            cflags: map_list(&m, "cflags"),
            ldflags: map_list(&m, "ldflags"),
            ldscript: map_str(&m, "linker_script"),
            entry: map_str(&m, "entry"),
            out: map_str(&m, "out"),
            platform: map_str(&m, "platform"),
            start_group: map_bool(&m, "start_group"),
            whole_archive: map_bool(&m, "whole_archive"),
            ..StarTargetDef::default()
        };
        t.src.retain(|s| !s.is_empty());
        c.borrow_mut().targets.push(t);
        Ok(Dynamic::UNIT)
    });

    let c = Rc::clone(&cfg);
    engine.register_fn("install", move |m: Map| -> R<Dynamic> {
        let name = map_str(&m, "target")
            .ok_or_else(|| "install(): missing target".to_string())?;
        let mut cfg = c.borrow_mut();
        let t = match cfg.targets.iter_mut().find(|t| t.name == name) {
            Some(t) => t,
            None => {
                return Err(format!("install(): unknown target '{}'", name).into());
            }
        };
        t.i_bin = map_list(&m, "bin");
        t.i_lib = map_list(&m, "lib");
        t.i_include = map_list(&m, "include");
        t.i_share = map_list(&m, "share");
        t.i_etc = map_list(&m, "etc");
        t.i_rootfs = map_str(&m, "rootfs");
        Ok(Dynamic::UNIT)
    });

    engine
        .run(&script)
        .map_err(|e| format!("{}: {}", path, e))?;
    let out = cfg.borrow().clone();
    Ok(out)
}
