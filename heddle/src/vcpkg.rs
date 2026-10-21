use crate::json::Json;
use crate::pkg;
use crate::sys;

struct Port {
    name: String,
    version: Option<String>,
    desc: Option<String>,
    homepage: Option<String>,
    deps: Vec<String>,
}

fn join(a: &str, b: &str) -> String {
    if a.is_empty() || a == "." {
        return b.to_string();
    }
    format!("{}/{}", a, b)
}

fn exists(p: &str) -> bool {
    sys::stat(p).is_some()
}

fn port_load(dir: &str) -> Result<Port, String> {
    let path = join(dir, "vcpkg.json");
    if !exists(&path) {
        return Err(format!(
            "no vcpkg.json in {}",
            if dir.is_empty() { "." } else { dir }
        ));
    }
    let j = Json::parse(&path)?;
    let name = match j.get("name") {
        Some(n) => n.to_string(),
        None => return Err("vcpkg.json has no name".to_string()),
    };
    let version = j.get("version").map(|s| s.to_string());
    let desc = j.get("description").map(|s| s.to_string());
    let homepage = j.get("homepage").map(|s| s.to_string());
    let mut deps: Vec<String> = Vec::new();
    let mut i = 0usize;
    loop {
        let key = format!("dependencies[{}]", i);
        if let Some(d) = j.get(&key) {
            deps.push(d.to_string());
            i += 1;
            continue;
        }
        let sub = format!("dependencies[{}].name", i);
        match j.get(&sub) {
            Some(n) => {
                deps.push(n.to_string());
                i += 1;
            }
            None => break,
        }
    }
    Ok(Port {
        name,
        version,
        desc,
        homepage,
        deps,
    })
}

const TRIPLETS: [(&str, &str, &str, &str); 13] = [
    ("x64-linux", "x86_64", "", ""),
    ("x86-linux", "i686", "", ""),
    ("arm64-linux", "aarch64", "", ""),
    ("arm-linux", "armv7a", "eabihf", "hard"),
    ("x64-windows", "x86_64", "", ""),
    ("x86-windows", "i686", "", ""),
    ("x64-windows-static", "x86_64", "", ""),
    ("x64-windows-static-md", "x86_64", "", ""),
    ("x64-osx", "x86_64", "", ""),
    ("arm64-osx", "aarch64", "", ""),
    ("thumbv7m-none-eabi", "armv7m", "eabi", "soft"),
    ("thumbv7em-none-eabihf", "armv7em", "eabihf", "hard"),
    ("arm-none-eabi", "armv7m", "eabi", "soft"),
];

fn triplet(name: &str) -> Option<&'static (&'static str, &'static str, &'static str, &'static str)> {
    TRIPLETS.iter().find(|t| t.0 == name)
}

const PAYLOAD: [&str; 4] = ["include", "lib", "bin", "share"];

fn stem(file: &str) -> Option<&str> {
    let base = match file.rfind('/') {
        Some(i) => &file[i + 1..],
        None => file,
    };
    if !base.starts_with("lib") {
        return None;
    }
    let dot = match base.rfind('.') {
        Some(i) => i,
        None => return None,
    };
    if dot <= 3 {
        return None;
    }
    if dot - 3 + 1 > 256 {
        return None;
    }
    Some(&base[3..dot])
}

fn write_libs(libdir: &str, meta: &str) -> i32 {
    let rd = match std::fs::read_dir(libdir) {
        Ok(v) => v,
        Err(_) => return 0,
    };
    let mut f = match std::fs::File::create(meta) {
        Ok(v) => v,
        Err(_) => return -1,
    };
    use std::io::Write;
    for e in rd.flatten() {
        let name = e.file_name();
        let name = name.to_string_lossy();
        if let Some(s) = stem(&name) {
            let _ = writeln!(f, "{}", s);
        }
    }
    0
}

fn port_version(p: &Port) -> &str {
    match &p.version {
        Some(v) if !v.is_empty() => v.as_str(),
        _ => "0",
    }
}

fn import(p: &Port, installed: &str, trip: &str, store: &str) -> Result<String, String> {
    let tree = join(installed, trip);
    if !sys::is_dir(&tree) {
        return Err(format!("no installed tree at {}", tree));
    }
    let d3 = format!("{}/library/{}/{}", store, p.name, port_version(p));
    sys::mkpath(&d3);
    let mut copied = 0;
    for item in PAYLOAD.iter() {
        let src = join(&tree, item);
        if sys::is_dir(&src) && pkg::copy_tree(&src, &join(&d3, item)) {
            copied += 1;
        }
    }
    if copied == 0 {
        return Err(format!(
            "installed tree {} has no include/lib/bin/share",
            trip
        ));
    }
    let meta = join(&d3, ".heddle-pkg");
    write_libs(&join(&d3, "lib"), &meta);
    Ok(pkg::hash_tree(&d3))
}

fn is_registry(dir: &str) -> bool {
    let p = join(dir, "ports");
    let v = join(dir, "versions");
    sys::is_dir(&p) && sys::is_dir(&v)
}

fn usage() {
    println!(
        "usage: heddle vcpkg <command> [options]\n\n  show <port-dir>                 read a vcpkg.json\n  triplet <name>                  print the [target] mapping\n  check <registry-dir>            test for a vcpkg registry\n  import <port-dir> [options]     import an installed tree\n\nimport options:\n  --from DIR      installed tree (default <port>/.vcpkg)\n  --triplet NAME  e.g. x64-linux\n  --store DIR     heddle store (default .heddle/store)\n  --lock FILE     update a heddle.lock (default ./heddle.lock)"
    );
}

fn opt<'a>(args: &'a [String], name: &str) -> Option<&'a str> {
    for i in 0..args.len() {
        if args[i] == name && i + 1 < args.len() {
            return Some(args[i + 1].as_str());
        }
        if args[i].starts_with(name) && args[i].as_bytes().get(name.len()) == Some(&b'=') {
            return Some(&args[i][name.len() + 1..]);
        }
    }
    None
}

fn cmd_show(dir: &str) -> i32 {
    let p = match port_load(dir) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };
    println!("name     {}", p.name);
    println!(
        "version  {}",
        match &p.version {
            Some(v) => v.as_str(),
            None => "-",
        }
    );
    if let Some(d) = &p.desc {
        println!("desc     {}", d);
    }
    if let Some(h) = &p.homepage {
        println!("home     {}", h);
    }
    for d in p.deps.iter() {
        println!("dep      {}", d);
    }
    0
}

fn cmd_triplet(name: &str) -> i32 {
    let t = match triplet(name) {
        Some(t) => t,
        None => {
            eprintln!("heddle: unknown triplet '{}'", name);
            return 1;
        }
    };
    println!("[target]");
    println!("arch = \"{}\"", t.1);
    if !t.2.is_empty() {
        println!("abi = \"{}\"", t.2);
    }
    if !t.3.is_empty() {
        println!("float = \"{}\"", t.3);
    }
    0
}

fn cmd_check(dir: &str) -> i32 {
    if !is_registry(dir) {
        eprintln!(
            "heddle: {} is not a vcpkg registry",
            if dir.is_empty() { "." } else { dir }
        );
        return 1;
    }
    println!("heddle: {} looks like a vcpkg registry", dir);
    0
}

fn cmd_import(args: &[String]) -> i32 {
    let from_opt = opt(args, "--from").map(|s| s.to_string());
    let triplet_opt = opt(args, "--triplet").map(|s| s.to_string());
    let store_opt = opt(args, "--store").map(|s| s.to_string());
    let lock_opt = opt(args, "--lock").map(|s| s.to_string());
    let mut port: Option<String> = None;
    let mut i = 3usize;
    while i < args.len() {
        let a = args[i].as_str();
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
        if port.is_none() {
            port = Some(a.to_string());
        }
        i += 1;
    }

    let port = match port {
        Some(p) => p,
        None => {
            eprintln!("heddle: import needs a port directory");
            return 2;
        }
    };

    let p = match port_load(&port) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };

    let trip = match &triplet_opt {
        Some(t) => t.clone(),
        None => {
            eprintln!("heddle: import needs --triplet");
            return 2;
        }
    };

    let from = match from_opt {
        Some(f) => f,
        None => join(&port, ".vcpkg"),
    };
    let store = match store_opt {
        Some(s) => s,
        None => ".heddle/store".to_string(),
    };

    let hash = match import(&p, &from, &trip, &store) {
        Ok(h) => h,
        Err(e) => {
            eprintln!("heddle: {}", e);
            return 1;
        }
    };

    println!(
        "heddle: imported {} {} ({})",
        p.name,
        match &p.version {
            Some(v) => v.as_str(),
            None => "-",
        },
        trip
    );
    println!(
        "heddle: store  {}/library/{}/{}",
        store,
        p.name,
        port_version(&p)
    );
    println!("heddle: sha256 {}", hash);

    let mut lock = lock_opt;
    if lock.is_none() && sys::stat("heddle.toml").is_some() {
        lock = Some("heddle.lock".to_string());
    }
    if let Some(l) = &lock {
        if let Err(e) = pkg::lock_add(l, "library", &p.name, port_version(&p), &hash) {
            eprintln!("heddle: {}", e);
            return 1;
        }
        println!("heddle: lock   {}", l);
    }
    0
}

pub fn heddle_vcpkg(args: &[String]) -> i32 {
    if args.len() < 3 {
        usage();
        return 2;
    }
    let cmd = args[2].as_str();
    if cmd == "show" {
        return cmd_show(args.get(3).map(|s| s.as_str()).unwrap_or("."));
    }
    if cmd == "triplet" {
        return cmd_triplet(args.get(3).map(|s| s.as_str()).unwrap_or(""));
    }
    if cmd == "check" {
        return cmd_check(args.get(3).map(|s| s.as_str()).unwrap_or("."));
    }
    if cmd == "import" {
        return cmd_import(args);
    }
    if cmd == "-h" || cmd == "--help" {
        usage();
        return 0;
    }
    eprintln!("heddle: unknown vcpkg command '{}'", cmd);
    usage();
    2
}
