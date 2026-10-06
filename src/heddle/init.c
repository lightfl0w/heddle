#include "init.h"

#include "sys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

static int g_star;

static char *join(const char *a, const char *b) {
    if (!a || !a[0] || !strcmp(a, ".")) return sys_dup(b);

    size_t n = strlen(a) + strlen(b) + 2;
    char  *p = (char *)malloc(n);

    if (p) snprintf(p, n, "%s/%s", a, b);

    return p;
}

static int write_file(const char *path, const char *text, int force) {
    SYS_STAT st;

    if (!force && sys_stat(path, &st) == 0) {
        fprintf(stderr, "heddle: %s exists, skip (use --force to overwrite)\n",
                path);
        return 0;
    }

    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", path);

    char *slash = strrchr(dir, '/');

    if (slash) { *slash = 0; sys_mkpath(dir); }

    FILE *f = fopen(path, "wb");

    if (!f) {
        fprintf(stderr, "heddle: cannot write %s\n", path);
        return -1;
    }

    fputs(text, f);
    fclose(f);

    printf("  create %s\n", path);
    return 0;
}

typedef struct {
    const char *name;
    const char *desc;
} INIT_TYPE;

static const INIT_TYPE g_types[] = {
    { "exe",       "host executable" },
    { "lib",       "static library with a public header" },
    { "embedded",  "cross-compiled firmware (pick an arch)" },
    { "baremetal", "bootloader + kernel + disk image" },
};

static int type_count(void) {
    return (int)(sizeof(g_types) / sizeof(g_types[0]));
}

static int type_index(const char *name) {
    for (int i = 0; i < type_count(); i++)
        if (!strcmp(g_types[i].name, name)) return i;

    return -1;
}

static void list_types(void) {
    printf("project types:\n");

    for (int i = 0; i < type_count(); i++)
        printf("  %-10s %s\n", g_types[i].name, g_types[i].desc);
}

static int is_tty_stdin(void) {
#if defined(_WIN32)
    return _isatty(_fileno(stdin));
#else
    return isatty(fileno(stdin));
#endif
}

static int choose_type(void) {
    printf("pick a project type:\n");

    for (int i = 0; i < type_count(); i++)
        printf("  %d) %-10s %s\n", i + 1, g_types[i].name, g_types[i].desc);

    printf("type [1]: ");
    fflush(stdout);

    char line[128];

    if (!fgets(line, sizeof(line), stdin)) return 0;

    int n = atoi(line);

    if (n >= 1 && n <= type_count()) return n - 1;

    for (int i = 0; i < type_count(); i++)
        if (!strncmp(line, g_types[i].name, strlen(g_types[i].name)))
            return i;

    return 0;
}

static const char *g_archs[] = {
    "armv7em", "armv7m", "armv6m", "armv8m", "cortex-m7",
    "aarch64", "riscv32imac", "riscv64", "xtensa", "avr", "msp430",
};

static int arch_count(void) {
    return (int)(sizeof(g_archs) / sizeof(g_archs[0]));
}

static const char *choose_arch(void) {
    printf("target arch:\n");

    for (int i = 0; i < arch_count(); i++)
        printf("  %d) %s\n", i + 1, g_archs[i]);

    printf("arch [1]: ");
    fflush(stdout);

    char line[128];

    if (!fgets(line, sizeof(line), stdin)) return g_archs[0];

    int n = atoi(line);

    if (n >= 1 && n <= arch_count()) return g_archs[n - 1];

    for (int i = 0; i < arch_count(); i++)
        if (!strncmp(line, g_archs[i], strlen(g_archs[i]))) return g_archs[i];

    return g_archs[0];
}

static int gen_exe(const char *dir, int force) {
    const char *toml =
        "[build]\n"
        "dir = \"out\"\n"
        "\n"
        "[toolchain.host]\n"
        "cc = \"cc\"\n"
        "cflags = [\"-O2\", \"-Wall\"]\n"
        "\n"
        "[target.app]\n"
        "type = \"exe\"\n"
        "src = [\"src/*.c\"]\n"
        "inc = [\"src\"]\n";

    const char *star =
        "build(dir = \"out\")\n"
        "\n"
        "target(\n"
        "    name = \"app\",\n"
        "    type = \"exe\",\n"
        "    src = glob(\"src/*.c\"),\n"
        "    inc = [\"src\"],\n"
        ")\n";

    const char *main_c =
        "#include <stdio.h>\n"
        "\n"
        "int main(void) {\n"
        "    printf(\"hello\\n\");\n"
        "    return 0;\n"
        "}\n";

    char *a = join(dir, g_star ? "heddle.star" : "heddle.toml");
    char *b = join(dir, "src/main.c");

    int rc = 0;

    if (!a || !b) rc = -1;
    else {
        if (write_file(a, g_star ? star : toml, force) != 0) rc = -1;
        if (write_file(b, main_c, force) != 0) rc = -1;
    }

    free(a);
    free(b);
    return rc;
}

static int gen_lib(const char *dir, int force) {
    const char *toml =
        "[build]\n"
        "dir = \"out\"\n"
        "\n"
        "[toolchain.host]\n"
        "cc = \"cc\"\n"
        "cflags = [\"-O2\", \"-Wall\"]\n"
        "\n"
        "[target.mylib]\n"
        "type = \"staticlib\"\n"
        "src = [\"src/*.c\"]\n"
        "inc = [\"include\"]\n"
        "\n"
        "[target.mylib.install]\n"
        "lib = \"out/libmylib.a\"\n"
        "include = [\"include/**/*.h\"]\n";

    const char *star =
        "build(dir = \"out\")\n"
        "\n"
        "target(\n"
        "    name = \"mylib\",\n"
        "    type = \"staticlib\",\n"
        "    src = glob(\"src/*.c\"),\n"
        "    inc = [\"include\"],\n"
        ")\n"
        "\n"
        "install(target = \"mylib\", lib = [\"out/libmylib.a\"], "
        "include = [\"include/**/*.h\"])\n";

    const char *h =
        "#ifndef MYLIB_H\n"
        "#define MYLIB_H\n"
        "\n"
        "int mylib(void);\n"
        "\n"
        "#endif\n";

    const char *c =
        "#include \"mylib.h\"\n"
        "\n"
        "int mylib(void) {\n"
        "    return 0;\n"
        "}\n";

    char *a = join(dir, g_star ? "heddle.star" : "heddle.toml");
    char *b = join(dir, "include/mylib.h");
    char *c2 = join(dir, "src/mylib.c");

    int rc = 0;

    if (!a || !b || !c2) rc = -1;
    else {
        if (write_file(a, g_star ? star : toml, force) != 0) rc = -1;
        if (write_file(b, h, force) != 0) rc = -1;
        if (write_file(c2, c, force) != 0) rc = -1;
    }

    free(a);
    free(b);
    free(c2);
    return rc;
}

static int is_arm(const char *arch) {
    return !strncmp(arch, "arm", 3) || !strncmp(arch, "cortex", 6);
}

static int gen_embedded(const char *dir, const char *arch, int force) {
    char toml[4096];
    char abi[512];

    if (is_arm(arch))
        snprintf(abi, sizeof(abi),
                 "abi = \"eabi\"\n"
                 "float = \"soft\"\n");
    else
        abi[0] = 0;

    snprintf(toml, sizeof(toml),
        "[build]\n"
        "dir = \"out\"\n"
        "\n"
        "[target]\n"
        "arch = \"%s\"\n"
        "%s"
        "\n"
        "[target.firmware]\n"
        "type = \"exe\"\n"
        "src = [\"src/*.c\"]\n"
        "inc = [\"src\"]\n"
        "linker_script = \"firmware.ld\"\n"
        "entry = \"reset_handler\"\n"
        "ldflags = [\"-nostdlib\", \"-nostartfiles\"]\n",
        arch, abi);

    const char *c =
        "#include <stdint.h>\n"
        "\n"
        "volatile uint32_t counter;\n"
        "\n"
        "void reset_handler(void) {\n"
        "    for (;;) counter++;\n"
        "}\n";

    const char *ld =
        "ENTRY(reset_handler)\n"
        "MEMORY\n"
        "{\n"
        "    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K\n"
        "    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K\n"
        "}\n"
        "SECTIONS\n"
        "{\n"
        "    .text : { *(.text*) *(.rodata*) } > FLASH\n"
        "    .data : { *(.data*) } > RAM\n"
        "    .bss  : { *(.bss*) *(COMMON) } > RAM\n"
        "}\n";

    char *a  = join(dir, "heddle.toml");
    char *b  = join(dir, "src/main.c");
    char *c2 = join(dir, "firmware.ld");

    int rc = 0;

    if (!a || !b || !c2) rc = -1;
    else {
        if (write_file(a, toml, force) != 0) rc = -1;
        if (write_file(b, c, force) != 0) rc = -1;
        if (write_file(c2, ld, force) != 0) rc = -1;
    }

    free(a);
    free(b);
    free(c2);
    return rc;
}

static int gen_baremetal(const char *dir, int force) {
    const char *toml =
        "[build]\n"
        "dir = \"out\"\n"
        "\n"
        "[toolchain.host]\n"
        "cc = \"cc\"\n"
        "as = \"nasm\"\n"
        "cflags = [\"-O2\", \"-ffreestanding\", \"-fno-pic\", "
        "\"-fno-stack-protector\", \"-m32\", \"-nostdlib\"]\n"
        "\n"
        "[target.mbr]\n"
        "type = \"raw\"\n"
        "src = [\"src/mbr.asm\"]\n"
        "format = \"bin\"\n"
        "out = \"out/mbr.bin\"\n"
        "\n"
        "[target.kernel]\n"
        "type = \"exe\"\n"
        "src = [\"src/boot.asm\", \"src/kernel.c\"]\n"
        "linker_script = \"kernel.ld\"\n"
        "ldflags = [\"-nostdlib\", \"-m32\", \"-Wl,--build-id=none\"]\n"
        "\n"
        "[target.image]\n"
        "type = \"custom\"\n"
        "cmd = \"cat out/mbr.bin out/kernel > out/os.img\"\n"
        "out = \"out/os.img\"\n"
        "deps = [\"mbr\", \"kernel\"]\n"
        "\n"
        "[target.image.install]\n"
        "bin = \"out/os.img\"\n";

    const char *mbr =
        "org 0x7c00\n"
        "bits 16\n"
        "\n"
        "start:\n"
        "    mov si, msg\n"
        ".loop:\n"
        "    lodsb\n"
        "    or al, al\n"
        "    jz .halt\n"
        "    mov ah, 0x0e\n"
        "    int 0x10\n"
        "    jmp .loop\n"
        ".halt:\n"
        "    cli\n"
        "    hlt\n"
        "    jmp .halt\n"
        "\n"
        "msg db \"heddle\", 0\n"
        "\n"
        "times 510-($-$$) db 0\n"
        "dw 0xaa55\n";

    const char *boot =
        "global _start\n"
        "extern kmain\n"
        "\n"
        "section .text\n"
        "_start:\n"
        "    call kmain\n"
        "    cli\n"
        ".hang:\n"
        "    hlt\n"
        "    jmp .hang\n";

    const char *kernel =
        "void kmain(void) {\n"
        "    for (;;) {\n"
        "    }\n"
        "}\n";

    const char *ld =
        "ENTRY(_start)\n"
        "SECTIONS {\n"
        "    . = 0x10000;\n"
        "    .text : { *(.text) }\n"
        "    .data : { *(.data) }\n"
        "    .bss  : { *(.bss) }\n"
        "}\n";

    char *a  = join(dir, "heddle.toml");
    char *b  = join(dir, "src/mbr.asm");
    char *c  = join(dir, "src/boot.asm");
    char *d  = join(dir, "src/kernel.c");
    char *e  = join(dir, "kernel.ld");

    int rc = 0;

    if (!a || !b || !c || !d || !e) rc = -1;
    else {
        if (write_file(a, toml, force) != 0) rc = -1;
        if (write_file(b, mbr, force) != 0) rc = -1;
        if (write_file(c, boot, force) != 0) rc = -1;
        if (write_file(d, kernel, force) != 0) rc = -1;
        if (write_file(e, ld, force) != 0) rc = -1;
    }

    free(a);
    free(b);
    free(c);
    free(d);
    free(e);
    return rc;
}

static void print_next_steps(const char *type, const char *dir) {
    const char *target = "app";

    if (!strcmp(type, "lib"))       target = "mylib";
    if (!strcmp(type, "embedded"))  target = "firmware";
    if (!strcmp(type, "baremetal")) target = "image";

    printf("\ndone. next:\n");

    if (strcmp(dir, "."))
        printf("  cd %s\n", dir);

    printf("  heddle %s\n", target);
    printf("  heddle check\n");
}

int heddle_init(int argc, char **argv) {
    const char *type  = NULL;
    const char *dir   = ".";
    const char *arch  = NULL;
    int         force = 0;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: heddle init [TYPE] [DIR]\n\n");
            list_types();
            printf("\noptions:\n"
                   "  --arch NAME   target arch for 'embedded'\n"
                   "  --star        write heddle.star instead of heddle.toml\n"
                   "  --force       overwrite existing files\n"
                   "  --list        list project types\n");
            return 0;
        }

        if (!strcmp(a, "--list")) { list_types(); return 0; }
        if (!strcmp(a, "--force")) { force = 1; continue; }
        if (!strcmp(a, "--star"))  { g_star = 1; continue; }

        if (!strncmp(a, "--arch=", 7)) { arch = a + 7; continue; }

        if (a[0] == '-') {
            fprintf(stderr, "heddle: unknown option '%s'\n", a);
            return 2;
        }

        if (!type) type = a;
        else       dir  = a;
    }

    if (type && type_index(type) < 0) {
        fprintf(stderr, "heddle: unknown project type '%s'\n\n", type);
        list_types();
        return 2;
    }

    if (!type) {
        if (is_tty_stdin()) {
            type = g_types[choose_type()].name;
        } else {
            type = "exe";
            fprintf(stderr, "heddle: no type given, using '%s'\n", type);
        }
    }

    if (!strcmp(type, "embedded")) {
        if (arch) {
            int ok = 0;

            for (int i = 0; i < arch_count(); i++)
                if (!strcmp(g_archs[i], arch)) ok = 1;

            if (!ok) {
                fprintf(stderr, "heddle: unknown arch '%s'\n", arch);
                return 2;
            }
        } else if (is_tty_stdin()) {
            arch = choose_arch();
        } else {
            arch = g_archs[0];
        }
    }

    printf("heddle: init %s in %s\n", type, dir);

    int rc = 0;

    if (!strcmp(type, "exe"))            rc = gen_exe(dir, force);
    else if (!strcmp(type, "lib"))       rc = gen_lib(dir, force);
    else if (!strcmp(type, "embedded"))  rc = gen_embedded(dir, arch, force);
    else if (!strcmp(type, "baremetal")) rc = gen_baremetal(dir, force);

    if (rc != 0) {
        fprintf(stderr, "heddle: init failed\n");
        return 1;
    }

    print_next_steps(type, dir);
    return 0;
}
