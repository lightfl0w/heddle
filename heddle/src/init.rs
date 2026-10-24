use crate::sys;

use std::io::IsTerminal;
use std::sync::atomic::{AtomicBool, Ordering};

static G_STAR: AtomicBool = AtomicBool::new(false);

struct InitType {
    name: &'static str,
    desc: &'static str,
}

const G_TYPES: [InitType; 4] = [
    InitType {
        name: "exe",
        desc: "host executable",
    },
    InitType {
        name: "lib",
        desc: "static library with a public header",
    },
    InitType {
        name: "embedded",
        desc: "cross-compiled firmware (pick an arch)",
    },
    InitType {
        name: "baremetal",
        desc: "bootloader + kernel + disk image",
    },
];

const G_ARCHS: [&str; 11] = [
    "armv7em",
    "armv7m",
    "armv6m",
    "armv8m",
    "cortex-m7",
    "aarch64",
    "riscv32imac",
    "riscv64",
    "xtensa",
    "avr",
    "msp430",
];

fn is_space(c: u8) -> bool {
    c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' || c == 0x0b || c == 0x0c
}

fn join(a: &str, b: &str) -> String {
    if a.is_empty() || a == "." {
        return b.to_string();
    }
    format!("{}/{}", a, b)
}

fn write_file(path: &str, text: &str, force: bool) -> i32 {
    if !force && sys::stat(path).is_some() {
        eprintln!("heddle: {} exists, skip (use --force to overwrite)", path);
        return 0;
    }
    if let Some(i) = path.rfind('/') {
        sys::mkpath(&path[..i]);
    }
    if std::fs::write(path, text.as_bytes()).is_err() {
        eprintln!("heddle: cannot write {}", path);
        return -1;
    }
    println!("  create {}", path);
    0
}

fn atoi(s: &str) -> i64 {
    let b = s.as_bytes();
    let mut i = 0usize;
    while i < b.len() && is_space(b[i]) {
        i += 1;
    }
    let mut neg = false;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        neg = b[i] == b'-';
        i += 1;
    }
    let mut n: i64 = 0;
    while i < b.len() && b[i].is_ascii_digit() {
        n = n.saturating_mul(10).saturating_add((b[i] - b'0') as i64);
        i += 1;
    }
    if neg {
        -n
    } else {
        n
    }
}

fn type_index(name: &str) -> Option<usize> {
    G_TYPES.iter().position(|t| t.name == name)
}

fn list_types() {
    println!("project types:");
    for t in G_TYPES.iter() {
        println!("  {:<10} {}", t.name, t.desc);
    }
}

fn is_tty_stdin() -> bool {
    std::io::stdin().is_terminal()
}

fn read_line() -> Option<String> {
    let mut line = String::new();
    match std::io::stdin().read_line(&mut line) {
        Ok(0) => None,
        Ok(_) => Some(line),
        Err(_) => None,
    }
}

fn choose_type() -> usize {
    println!("pick a project type:");
    for (i, t) in G_TYPES.iter().enumerate() {
        println!("  {}) {:<10} {}", i + 1, t.name, t.desc);
    }
    print!("type [1]: ");
    let _ = std::io::Write::flush(&mut std::io::stdout());
    let line = match read_line() {
        Some(l) => l,
        None => return 0,
    };
    let n = atoi(&line);
    if n >= 1 && n <= G_TYPES.len() as i64 {
        return (n - 1) as usize;
    }
    for (i, t) in G_TYPES.iter().enumerate() {
        if line.starts_with(t.name) {
            return i;
        }
    }
    0
}

fn choose_arch() -> &'static str {
    println!("target arch:");
    for (i, a) in G_ARCHS.iter().enumerate() {
        println!("  {}) {}", i + 1, a);
    }
    print!("arch [1]: ");
    let _ = std::io::Write::flush(&mut std::io::stdout());
    let line = match read_line() {
        Some(l) => l,
        None => return G_ARCHS[0],
    };
    let n = atoi(&line);
    if n >= 1 && n <= G_ARCHS.len() as i64 {
        return G_ARCHS[(n - 1) as usize];
    }
    for a in G_ARCHS.iter() {
        if line.starts_with(a) {
            return a;
        }
    }
    G_ARCHS[0]
}

const EXE_TOML: &str = concat!(
    "[build]\n",
    "dir = \"out\"\n",
    "\n",
    "[toolchain.host]\n",
    "cc = \"cc\"\n",
    "cflags = [\"-O2\", \"-Wall\"]\n",
    "\n",
    "[target.app]\n",
    "type = \"exe\"\n",
    "src = [\"src/*.c\"]\n",
    "inc = [\"src\"]\n",
);

const EXE_STAR: &str = concat!(
    "build(dir = \"out\")\n",
    "\n",
    "target(\n",
    "    name = \"app\",\n",
    "    type = \"exe\",\n",
    "    src = glob(\"src/*.c\"),\n",
    "    inc = [\"src\"],\n",
    ")\n",
);

const EXE_MAIN_C: &str = concat!(
    "#include <stdio.h>\n",
    "\n",
    "int main(void) {\n",
    "    printf(\"hello\\n\");\n",
    "    return 0;\n",
    "}\n",
);

const LIB_TOML: &str = concat!(
    "[build]\n",
    "dir = \"out\"\n",
    "\n",
    "[toolchain.host]\n",
    "cc = \"cc\"\n",
    "cflags = [\"-O2\", \"-Wall\"]\n",
    "\n",
    "[target.mylib]\n",
    "type = \"staticlib\"\n",
    "src = [\"src/*.c\"]\n",
    "inc = [\"include\"]\n",
    "\n",
    "[target.mylib.install]\n",
    "lib = \"out/libmylib.a\"\n",
    "include = [\"include/**/*.h\"]\n",
);

const LIB_STAR: &str = concat!(
    "build(dir = \"out\")\n",
    "\n",
    "target(\n",
    "    name = \"mylib\",\n",
    "    type = \"staticlib\",\n",
    "    src = glob(\"src/*.c\"),\n",
    "    inc = [\"include\"],\n",
    ")\n",
    "\n",
    "install(target = \"mylib\", lib = [\"out/libmylib.a\"], include = [\"include/**/*.h\"])\n",
);

const LIB_H: &str = concat!(
    "#ifndef MYLIB_H\n",
    "#define MYLIB_H\n",
    "\n",
    "int mylib(void);\n",
    "\n",
    "#endif\n",
);

const LIB_C: &str = concat!(
    "#include \"mylib.h\"\n",
    "\n",
    "int mylib(void) {\n",
    "    return 0;\n",
    "}\n",
);

fn gen_exe(dir: &str, force: bool) -> i32 {
    let star = G_STAR.load(Ordering::Relaxed);
    let a = join(dir, if star { "heddle.star" } else { "heddle.toml" });
    let b = join(dir, "src/main.c");
    let mut rc = 0;
    if write_file(&a, if star { EXE_STAR } else { EXE_TOML }, force) != 0 {
        rc = -1;
    }
    if write_file(&b, EXE_MAIN_C, force) != 0 {
        rc = -1;
    }
    rc
}

fn gen_lib(dir: &str, force: bool) -> i32 {
    let star = G_STAR.load(Ordering::Relaxed);
    let a = join(dir, if star { "heddle.star" } else { "heddle.toml" });
    let b = join(dir, "include/mylib.h");
    let c = join(dir, "src/mylib.c");
    let mut rc = 0;
    if write_file(&a, if star { LIB_STAR } else { LIB_TOML }, force) != 0 {
        rc = -1;
    }
    if write_file(&b, LIB_H, force) != 0 {
        rc = -1;
    }
    if write_file(&c, LIB_C, force) != 0 {
        rc = -1;
    }
    rc
}

fn is_arm(arch: &str) -> bool {
    arch.starts_with("arm") || arch.starts_with("cortex")
}

fn gen_embedded(dir: &str, arch: &str, force: bool) -> i32 {
    let abi = if is_arm(arch) {
        "abi = \"eabi\"\nfloat = \"soft\"\n"
    } else {
        ""
    };
    let toml = format!(
        concat!(
            "[build]\n",
            "dir = \"out\"\n",
            "\n",
            "[target]\n",
            "arch = \"{}\"\n",
            "{}\n",
            "[target.firmware]\n",
            "type = \"exe\"\n",
            "src = [\"src/*.c\"]\n",
            "inc = [\"src\"]\n",
            "linker_script = \"firmware.ld\"\n",
            "entry = \"reset_handler\"\n",
            "ldflags = [\"-nostdlib\", \"-nostartfiles\"]\n",
        ),
        arch, abi
    );
    let c = concat!(
        "#include <stdint.h>\n",
        "\n",
        "volatile uint32_t counter;\n",
        "\n",
        "void reset_handler(void) {\n",
        "    for (;;) counter++;\n",
        "}\n",
    );
    let ld = concat!(
        "ENTRY(reset_handler)\n",
        "MEMORY\n",
        "{\n",
        "    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K\n",
        "    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K\n",
        "}\n",
        "SECTIONS\n",
        "{\n",
        "    .text : { *(.text*) *(.rodata*) } > FLASH\n",
        "    .data : { *(.data*) } > RAM\n",
        "    .bss  : { *(.bss*) *(COMMON) } > RAM\n",
        "}\n",
    );
    let a = join(dir, "heddle.toml");
    let b = join(dir, "src/main.c");
    let c2 = join(dir, "firmware.ld");
    let mut rc = 0;
    if write_file(&a, &toml, force) != 0 {
        rc = -1;
    }
    if write_file(&b, c, force) != 0 {
        rc = -1;
    }
    if write_file(&c2, ld, force) != 0 {
        rc = -1;
    }
    rc
}

fn gen_baremetal(dir: &str, force: bool) -> i32 {
    let toml = concat!(
        "[build]\n",
        "dir = \"out\"\n",
        "\n",
        "[toolchain.host]\n",
        "cc = \"cc\"\n",
        "as = \"nasm\"\n",
        "cflags = [\"-O2\", \"-ffreestanding\", \"-fno-pic\", \"-fno-stack-protector\", \"-m32\", ",
        "\"-nostdlib\"]\n",
        "\n",
        "[target.mbr]\n",
        "type = \"raw\"\n",
        "src = [\"src/mbr.asm\"]\n",
        "format = \"bin\"\n",
        "out = \"out/mbr.bin\"\n",
        "\n",
        "[target.kernel]\n",
        "type = \"exe\"\n",
        "src = [\"src/boot.asm\", \"src/kernel.c\"]\n",
        "linker_script = \"kernel.ld\"\n",
        "ldflags = [\"-nostdlib\", \"-m32\", \"-Wl,--build-id=none\"]\n",
        "\n",
        "[target.image]\n",
        "type = \"custom\"\n",
        "cmd = \"cat out/mbr.bin out/kernel > out/os.img\"\n",
        "out = \"out/os.img\"\n",
        "deps = [\"mbr\", \"kernel\"]\n",
        "\n",
        "[target.image.install]\n",
        "bin = \"out/os.img\"\n",
    );
    let mbr = concat!(
        "org 0x7c00\n",
        "bits 16\n",
        "\n",
        "start:\n",
        "    mov si, msg\n",
        ".loop:\n",
        "    lodsb\n",
        "    or al, al\n",
        "    jz .halt\n",
        "    mov ah, 0x0e\n",
        "    int 0x10\n",
        "    jmp .loop\n",
        ".halt:\n",
        "    cli\n",
        "    hlt\n",
        "    jmp .halt\n",
        "\n",
        "msg db \"heddle\", 0\n",
        "\n",
        "times 510-($-$$) db 0\n",
        "dw 0xaa55\n",
    );
    let boot = concat!(
        "global _start\n",
        "extern kmain\n",
        "\n",
        "section .text\n",
        "_start:\n",
        "    call kmain\n",
        "    cli\n",
        ".hang:\n",
        "    hlt\n",
        "    jmp .hang\n",
    );
    let kernel = concat!(
        "void kmain(void) {\n",
        "    for (;;) {\n",
        "    }\n",
        "}\n",
    );
    let ld = concat!(
        "ENTRY(_start)\n",
        "SECTIONS {\n",
        "    . = 0x10000;\n",
        "    .text : { *(.text) }\n",
        "    .data : { *(.data) }\n",
        "    .bss  : { *(.bss) }\n",
        "}\n",
    );
    let a = join(dir, "heddle.toml");
    let b = join(dir, "src/mbr.asm");
    let c = join(dir, "src/boot.asm");
    let d = join(dir, "src/kernel.c");
    let e = join(dir, "kernel.ld");
    let mut rc = 0;
    if write_file(&a, toml, force) != 0 {
        rc = -1;
    }
    if write_file(&b, mbr, force) != 0 {
        rc = -1;
    }
    if write_file(&c, boot, force) != 0 {
        rc = -1;
    }
    if write_file(&d, kernel, force) != 0 {
        rc = -1;
    }
    if write_file(&e, ld, force) != 0 {
        rc = -1;
    }
    rc
}

fn print_next_steps(kind: &str, dir: &str) {
    let mut target = "app";
    if kind == "lib" {
        target = "mylib";
    }
    if kind == "embedded" {
        target = "firmware";
    }
    if kind == "baremetal" {
        target = "image";
    }

    println!("\ndone. next:");
    if dir != "." {
        println!("  cd {}", dir);
    }
    println!("  heddle {}", target);
    println!("  heddle check");
}

pub fn heddle_init(args: &[String]) -> i32 {
    let mut kind: Option<String> = None;
    let mut dir = String::from(".");
    let mut arch: Option<String> = None;
    let mut force = false;
    let mut i = 2usize;
    while i < args.len() {
        let a = args[i].as_str();
        if a == "-h" || a == "--help" {
            println!("usage: heddle init [TYPE] [DIR]\n");
            list_types();
            println!(
                "\noptions:\n  --arch NAME   target arch for 'embedded'\n  --star        write heddle.star instead of heddle.toml\n  --force       overwrite existing files\n  --list        list project types"
            );
            return 0;
        }
        if a == "--list" {
            list_types();
            return 0;
        }
        if a == "--force" {
            force = true;
            i += 1;
            continue;
        }
        if a == "--star" {
            G_STAR.store(true, Ordering::Relaxed);
            i += 1;
            continue;
        }
        if let Some(v) = a.strip_prefix("--arch=") {
            arch = Some(v.to_string());
            i += 1;
            continue;
        }
        if a.starts_with('-') {
            eprintln!("heddle: unknown option '{}'", a);
            return 2;
        }
        if kind.is_none() {
            kind = Some(a.to_string());
        } else {
            dir = a.to_string();
        }
        i += 1;
    }

    if let Some(k) = &kind {
        if type_index(k).is_none() {
            eprintln!("heddle: unknown project type '{}'\n", k);
            list_types();
            return 2;
        }
    }

    let kind = match kind {
        Some(k) => k,
        None => {
            if is_tty_stdin() {
                G_TYPES[choose_type()].name.to_string()
            } else {
                eprintln!("heddle: no type given, using 'exe'");
                "exe".to_string()
            }
        }
    };

    if kind == "embedded" {
        match &arch {
            Some(a) => {
                if !G_ARCHS.iter().any(|x| x == a) {
                    eprintln!("heddle: unknown arch '{}'", a);
                    return 2;
                }
            }
            None => {
                if is_tty_stdin() {
                    arch = Some(choose_arch().to_string());
                } else {
                    arch = Some(G_ARCHS[0].to_string());
                }
            }
        }
    }

    println!("heddle: init {} in {}", kind, dir);
    let mut rc = 0;
    if kind == "exe" {
        rc = gen_exe(&dir, force);
    } else if kind == "lib" {
        rc = gen_lib(&dir, force);
    } else if kind == "embedded" {
        rc = gen_embedded(&dir, arch.as_deref().unwrap_or(G_ARCHS[0]), force);
    } else if kind == "baremetal" {
        rc = gen_baremetal(&dir, force);
    }

    if rc != 0 {
        eprintln!("heddle: init failed");
        return 1;
    }

    print_next_steps(&kind, &dir);
    0
}
