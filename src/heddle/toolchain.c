#include "toolchain.h"
#include "proc.h"
#include "sys.h"
#include "toml.h"
#include "vswhere.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define EXEC_OK(path) (_access(path, 0) == 0)
#else
#include <unistd.h>
#define EXEC_OK(path) (access(path, X_OK) == 0)
#endif

typedef struct {
    const char *name;
    const char *family;
    const char *cc;
    const char *cxx;
    const char *as;
    const char *ar;
    const char *objext;
    const char *binext;
    const char *libext;
    const char *dllpre;
    const char *dllext;
    const char *soflag;
} TC_PRESET;

static const TC_PRESET g_presets[] = {
    {"host", "gnu", "cc", "c++", "nasm", "ar", ".o", "", ".a", "lib", ".so", "-shared"},
    {"linux", "gnu", "cc", "c++", "nasm", "ar", ".o", "", ".a", "lib", ".so", "-shared"},
    {"gcc", "gnu", "gcc", "g++", "nasm", "ar", ".o", "", ".a", "lib", ".so", "-shared"},
    {"clang", "gnu", "clang", "clang++", "nasm", "ar", ".o", "", ".a", "lib", ".so", "-shared"},
    {"macos", "gnu", "cc", "c++", "nasm", "ar", ".o", "", ".a", "lib", ".dylib", "-dynamiclib"},
    {"armcc", "armcc", "armcc", "armcc", "armasm", "armar", ".o", "", ".lib", "", ".dll", ""},
    {"iar", "iar", "iccarm", "iccarm", "iasm", "iarchive", ".o", "", ".a", "", ".dll", ""},
    {"mingw", "gnu", "x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-g++", "x86_64-w64-mingw32-nasm",
     "x86_64-w64-mingw32-ar", ".obj", ".exe", ".a", "lib", ".dll", "-shared"},
    {"msvc", "msvc", "cl", "cl", "nasm", "lib", ".obj", ".exe", ".lib", "", ".dll", "-shared"},
};

static const char *const g_auto[] = {"gcc", "clang", "tcc", "msvc"};

#if defined(_WIN32)
#define DIR_SEP "\\"

static int has_ext(const char *prog) {
    const char *slash = strrchr(prog, '/');
    const char *bs    = strrchr(prog, '\\');
    const char *base  = slash;
    if (bs && (!base || bs > base)) base = bs;
    base = base ? base + 1 : prog;
    return strchr(base, '.') != NULL;
}
#else
#define DIR_SEP "/"
#endif

static size_t dir_len(const char *dir) {
    size_t n = strlen(dir);
    while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) {
        if ((n == 3 && dir[1] == ':') || dir[n - 2] == ':') break;

        n--;
    }

    return n;
}

static int exec_try(const char *dir, const char *prog) {
    char        full[1024];
    size_t      n   = dir_len(dir);
    const char *sep = (n && dir[n - 1] != '/' && dir[n - 1] != '\\') ? DIR_SEP : "";
    snprintf(full, sizeof(full), "%.*s%s%s", (int)n, dir, sep, prog);
    if (EXEC_OK(full)) return 1;

#if defined(_WIN32)
    if (!has_ext(prog)) {
        snprintf(full, sizeof(full), "%.*s%s%s.exe", (int)n, dir, sep, prog);
        if (EXEC_OK(full)) return 1;
    }
#endif

    return 0;
}

static int on_path(const char *prog) {
    if (strchr(prog, '/') || strchr(prog, '\\')) return exec_try("", prog);

    const char *path = getenv("PATH");
    if (!path) return 0;

    char *copy = sys_dup(path);
    if (!copy) return 0;

    int   found = 0;
    char *dir   = copy;
    while (dir && !found) {
#if defined(_WIN32)
        char *sep = strchr(dir, ';');
#else
        char *sep = strchr(dir, ':');
#endif
        char keep = 0;
        if (sep) {
            keep = *sep;
            *sep = 0;
        }

        if (dir[0]) found = exec_try(dir, prog);

        if (sep) *sep = keep;
        if (!sep) break;

        dir = sep + 1;
    }

    free(copy);
    return found;
}

void tc_add_env_raw(TOOLCHAIN *tc, const char *s) {
    char **next = (char **)realloc(tc->env, sizeof(char *) * (size_t)(tc->nenv + 1));
    if (!next) return;

    tc->env           = next;
    tc->env[tc->nenv] = sys_dup(s);
    if (tc->env[tc->nenv]) tc->nenv++;
}

void tc_add_env(TOOLCHAIN *tc, const char *fmt, ...) {
    char    buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tc_add_env_raw(tc, buf);
}

static const TC_PRESET *preset_of(const char *name) {
    if (!name || !name[0]) return NULL;

    for (int i = 0; i < (int)(sizeof(g_presets) / sizeof(g_presets[0])); i++)
        if (!strcmp(g_presets[i].name, name)) return &g_presets[i];

    return NULL;
}

static const char *env_lookup(char **items, int n, const char *key) {
    size_t klen = strlen(key);
    for (int i = 0; i < n; i++) {
        if (strncmp(items[i], key, klen) == 0 && items[i][klen] == '=') return items[i] + klen + 1;
    }

    return NULL;
}

static int tc_file_exists(const char *path) {
    SYS_STAT st;
    return sys_stat(path, &st) == 0;
}

static int cl_in(const char *dir, size_t dl, char *out, size_t cap) {
    if (!dl || dl + 12 >= cap) return 0;

    snprintf(out, cap, "%.*s\\cl.exe", (int)dl, dir);
    if (!tc_file_exists(out)) {
        out[0] = 0;
        return 0;
    }

    return 1;
}

static void find_cl(char **items, int n, const char *arch, char *out, size_t cap) {
    out[0]            = 0;
    const char *vcdir = env_lookup(items, n, "VCToolsInstallDir");
    if (vcdir && vcdir[0]) {
        size_t l = strlen(vcdir);
        while (l > 0 && (vcdir[l - 1] == '\\' || vcdir[l - 1] == '/')) l--;

        snprintf(out, cap, "%.*s\\bin\\Host%s\\%s\\cl.exe", (int)l, vcdir, arch, arch);
        if (!tc_file_exists(out)) out[0] = 0;
    }

    for (const char *p = env_lookup(items, n, "PATH"); p && *p && !out[0];) {
        const char *e = strchr(p, ';');
        cl_in(p, e ? (size_t)(e - p) : strlen(p), out, cap);
        p = e ? e + 1 : NULL;
    }
}

static int msvc_env(TOOLCHAIN *tc, const char *arch, const char *want, char *cl_out,
                    size_t cl_cap) {
    char install[1024];
    if (vs_install(install, sizeof(install)) != 0) return -1;

    static char raw[131072];
    char       *items[1024];
    int         n;
    if (vs_env_capture(install, arch, want, raw, sizeof(raw)) != 0 ||
        (n = vs_env_split(raw, items, 1024)) == 0)
        return -1;

    const char *vc_inc = env_lookup(items, n, "INCLUDE");
    if (!vc_inc || !vc_inc[0]) return -1;

    for (int i = 0; i < n; i++) tc_add_env_raw(tc, items[i]);

    if (cl_out && cl_cap) find_cl(items, n, arch, cl_out, cl_cap);

    return 0;
}

static int msvc_fill(TOOLCHAIN *tc, const char *arch, const char *want, char *err, size_t errsz) {
    char cl[2048] = {0};
    if (msvc_env(tc, arch, want, cl, sizeof(cl)) != 0) {
        snprintf(err, errsz,
                 "MSVC environment could not be loaded; vcvarsall.bat was not "
                 "found or failed to run");
        return -1;
    }

    if (!cl[0]) {
        snprintf(err, errsz,
                 "cl.exe was not found in the Visual Studio environment; "
                 "check that the MSVC build tools are installed");
        return -1;
    }

    free(tc->cc);
    free(tc->cxx);
    free(tc->ar);
    free(tc->ld);
    tc->cc  = sys_dup(cl);
    tc->cxx = sys_dup(cl);
    tc->ld  = sys_dup(cl);
    tc->ar  = sys_dup("lib");
    return 0;
}

int tc_probe(const char *preset) {
    const TC_PRESET *p = preset_of(preset);
    if (!p) return 0;

    if (!strcmp(preset, "msvc")) {
        char install[1024];
        return vs_install(install, sizeof(install)) == 0;
    }

    return on_path(p->cc);
}

const char *tc_preset_cc(const char *preset) {
    const TC_PRESET *p = preset_of(preset);
    return p ? p->cc : "";
}

static const char *probe_tmpdir(void) {
#if defined(_WIN32)
    const char *t = getenv("TEMP");
    if (t && *t) return t;

    t = getenv("TMP");
    if (t && *t) return t;

    return ".";
#else
    const char *t = getenv("TMPDIR");
    if (t && *t) return t;

    return "/tmp";
#endif
}

int tc_probe_triple(const char *cc, char *out, size_t cap) {
    char log[4096];
    snprintf(log, sizeof(log), "%s/heddle-triple.log", probe_tmpdir());
    char *argv[4];
    argv[0] = (char *)cc;
    argv[1] = (char *)"-dumpmachine";
    argv[2] = NULL;
    PROC_RESULT r;
    memset(&r, 0, sizeof(r));
    out[0] = 0;
    if (proc_run(argv, NULL, log, NULL, 0, &r) != 0 || r.exit_code != 0) return -1;

    FILE *f = fopen(log, "rb");
    if (!f) return -1;

    char line[1024];
    int  got = 0;
    if (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;

        if (n) {
            if (n >= cap) n = cap - 1;
            memcpy(out, line, n);
            out[n] = 0;
            got    = 1;
        }
    }

    fclose(f);
    remove(log);
    return got ? 0 : -1;
}

int tc_triple_needs_msvc(const char *triple) {
    if (!triple || !*triple) return 0;

    if (!strstr(triple, "windows")) return 0;
    if (strstr(triple, "msvc")) return 1;

    return 0;
}

static int maybe_load_msvc_env(TOOLCHAIN *tc, const char *arch, char *err, size_t errsz) {
    if (!tc->cc || !tc->cc[0]) return 0;
    if (tc->family && !strcmp(tc->family, "msvc")) return 0;

    char triple[256];
    if (tc_probe_triple(tc->cc, triple, sizeof(triple)) != 0) return 0;
    if (!tc_triple_needs_msvc(triple)) return 0;

    if (msvc_env(tc, arch, NULL, NULL, 0) != 0) {
        snprintf(err, errsz,
                 "compiler '%s' targets the MSVC ABI (%s) but the Visual Studio "
                 "environment could not be loaded; vcvarsall.bat was not found "
                 "or failed to run",
                 tc->cc, triple);
        return -1;
    }

    return 0;
}

int tc_auto_count(void) {
    return (int)(sizeof(g_auto) / sizeof(g_auto[0]));
}

const char *tc_auto_at(int i) {
    if (i < 0 || i >= tc_auto_count()) return NULL;

    return g_auto[i];
}

static const char *or_default(const TOML *t, const char *sect, const char *key,
                              const char *fallback) {
    const char *v = toml_str(t, sect, key);
    return v ? v : fallback;
}

static char *dup_or(const TOML *t, const char *sect, const char *key, const char *fallback) {
    return sys_dup(or_default(t, sect, key, fallback));
}

static void add_flag(char ***arr, int *n, const char *v) {
    char **next = (char **)realloc(*arr, sizeof(char *) * (size_t)(*n + 1));
    if (!next) return;

    *arr       = next;
    (*arr)[*n] = sys_dup(v);
    (*n)++;
}

static void add_flags(TOML *t, const char *sect, const char *key, char ***arr, int *n) {
    for (int i = 0;; i++) {
        const char *v = toml_arr(t, sect, key, i);
        if (!v) break;

        add_flag(arr, n, v);
    }
}

static int arch_sel_flag(const char *f) {
    static const char *const prefixes[] = {"-march=", "-mabi=", "-mcpu=", "-mtune="};
    if (!f || f[0] != '-') return 0;

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (!strncmp(f, prefixes[i], strlen(prefixes[i]))) return 1;

    return !strcmp(f, "-m32") || !strcmp(f, "-m64") || !strcmp(f, "-m16") || !strcmp(f, "-mx32");
}

static void add_arch_ldflags(TOOLCHAIN *tc, const char *tok) {
    if (!arch_sel_flag(tok)) return;
    if (tc->family && strcmp(tc->family, "gnu") && strcmp(tc->family, "gcc") &&
        strcmp(tc->family, "clang"))
        return;

    for (int i = 0; i < tc->nldflags; i++)
        if (!strcmp(tc->ldflags[i], tok)) return;

    add_flag(&tc->ldflags, &tc->nldflags, tok);
}

static int is_auto(const char *name) {
    return !name || !*name || !strcmp(name, "auto") || !strcmp(name, "native");
}

static void read_flags(const char *dir, TOOLCHAIN *tc, const char *user) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/heddle.toml", dir);
    TOML t;
    char err[256];
    toml_init(&t);
    if (toml_parse(&t, path, err, sizeof(err)) == 0) {
        char sect[256];
        snprintf(sect, sizeof(sect), "toolchain.%s", user);
        const char *cc = toml_str(&t, sect, "cc");
        if (cc) {
            free(tc->cc);
            tc->cc = sys_dup(cc);
        }

        const char *as = toml_str(&t, sect, "as");
        if (as) {
            free(tc->as);
            tc->as = sys_dup(as);
        }

        add_flags(&t, sect, "cflags", &tc->cflags, &tc->ncflags);
    }

    toml_free(&t);
}

static void host_fix(TOOLCHAIN *tc) {
#if defined(_WIN32)
    if (tc->family && !strcmp(tc->family, "gnu")) {
        if (tc->binext && !tc->binext[0]) {
            free(tc->binext);
            tc->binext = sys_dup(".exe");
        }

        if (tc->soflag && !strcmp(tc->soflag, "-shared") && tc->dllpre &&
            !strcmp(tc->dllpre, "lib")) {
            free(tc->dllpre);
            tc->dllpre = sys_dup("");
        }
    }
#else
    (void)tc;
#endif
}

static void load_preset(TOOLCHAIN *tc, const TC_PRESET *p) {
    memset(tc, 0, sizeof(*tc));
    tc->name     = sys_dup(p->name);
    tc->family   = sys_dup(p->family);
    tc->cc       = sys_dup(p->cc);
    tc->cxx      = sys_dup(p->cxx);
    tc->as       = sys_dup(p->as);
    tc->ar       = sys_dup(p->ar);
    tc->ld       = sys_dup(p->cc);
    tc->objext   = sys_dup(p->objext);
    tc->binext   = sys_dup(p->binext);
    tc->libext   = sys_dup(p->libext);
    tc->dllpre   = sys_dup(p->dllpre);
    tc->dllext   = sys_dup(p->dllext);
    tc->soflag   = sys_dup(p->soflag);
    tc->platform = sys_dup(p->name);
    host_fix(tc);
}

static int apply_auto(TOOLCHAIN *tc, const char *dir, const char *user, const char *arch, char *err,
                      size_t errsz) {
    for (int i = 0; i < tc_auto_count(); i++) {
        const TC_PRESET *p = preset_of(g_auto[i]);
        if (!p || !tc_probe(g_auto[i])) continue;

        load_preset(tc, p);
        if (!strcmp(p->name, "msvc")) {
            if (msvc_fill(tc, arch, NULL, err, errsz) != 0) return -1;
        } else {
            if (maybe_load_msvc_env(tc, arch, err, errsz) != 0) return -1;
        }

        read_flags(dir, tc, user);
        return 0;
    }

    return -1;
}

static const char *msvc_arch(const char *arch) {
    if (!arch || !*arch) return "x64";

    if (!strcmp(arch, "x86_64") || !strcmp(arch, "amd64")) return "x64";
    if (!strcmp(arch, "i686") || !strcmp(arch, "i386") || !strcmp(arch, "x86")) return "x86";
    if (!strcmp(arch, "aarch64") || !strcmp(arch, "arm64")) return "arm64";
    if (!strncmp(arch, "armv", 4) || !strncmp(arch, "cortex", 6) || !strcmp(arch, "arm"))
        return "arm";

    return arch;
}

int tc_load(TOOLCHAIN *tc, const char *dir, const char *name, char *err, size_t errsz) {
    return tc_load_ex(tc, dir, name, NULL, NULL, NULL, NULL, err, errsz);
}

static char *apply_prefix(const char *prefix, const char *tool) {
    if (!prefix || !prefix[0] || !tool || !tool[0]) return sys_dup(tool);
    if (strchr(tool, '/') || strchr(tool, '\\')) return sys_dup(tool);
    if (!strncmp(tool, prefix, strlen(prefix))) return sys_dup(tool);

    size_t n = strlen(prefix) + strlen(tool) + 1;
    char  *p = (char *)malloc(n);
    if (p) snprintf(p, n, "%s%s", prefix, tool);

    return p;
}

int tc_load_ex(TOOLCHAIN *tc, const char *dir, const char *name, const char *prefix,
               const char *sysroot, const char *extra_cflags, const char *arch, char *err,
               size_t errsz) {
    const char *arch_msvc = msvc_arch(arch);
    memset(tc, 0, sizeof(*tc));
    if (is_auto(name)) {
        if (prefix && prefix[0]) {
            char cc[512];
            snprintf(cc, sizeof(cc), "%sgcc", prefix);
            if (on_path(cc)) {
                load_preset(tc, preset_of("gcc"));
                free(tc->name);
                free(tc->cc);
                free(tc->cxx);
                free(tc->ar);
                free(tc->ld);
                tc->name = sys_dup(prefix);
                tc->cc   = sys_dup(cc);
                tc->cxx  = apply_prefix(prefix, "g++");
                tc->ar   = apply_prefix(prefix, "ar");
                tc->ld   = sys_dup(cc);
                read_flags(dir, tc, "host");
                goto overlay;
            }
        }

        if (apply_auto(tc, dir, "host", arch_msvc, err, errsz) == 0) goto overlay;

        snprintf(err, errsz,
                 "no C compiler found on PATH; "
                 "set [build] toolchain or -t <name>");
        return -1;
    }

    char path[2048];
    snprintf(path, sizeof(path), "%s/heddle.toml", dir);
    TOML t;
    toml_init(&t);
    char perr[256];
    toml_parse(&t, path, perr, sizeof(perr));
    char sect[256];
    snprintf(sect, sizeof(sect), "toolchain.%s", name);
    const char      *base = or_default(&t, sect, "based_on", name);
    const TC_PRESET *p    = preset_of(base);
    if (!p) {
        snprintf(err, errsz, "unknown toolchain '%s'", base);
        toml_free(&t);
        return -1;
    }

    tc->name     = sys_dup(name);
    tc->family   = dup_or(&t, sect, "family", p->family);
    tc->cc       = dup_or(&t, sect, "cc", p->cc);
    tc->cxx      = dup_or(&t, sect, "cxx", p->cxx);
    tc->ar       = dup_or(&t, sect, "ar", p->ar);
    tc->ld       = dup_or(&t, sect, "ld", p->cc);
    tc->as       = dup_or(&t, sect, "as", p->as);
    tc->objext   = dup_or(&t, sect, "objext", p->objext);
    tc->binext   = dup_or(&t, sect, "binext", p->binext);
    tc->libext   = dup_or(&t, sect, "libext", p->libext);
    tc->dllpre   = dup_or(&t, sect, "dllpre", p->dllpre);
    tc->dllext   = dup_or(&t, sect, "dllext", p->dllext);
    tc->soflag   = dup_or(&t, sect, "soflag", p->soflag);
    tc->platform = dup_or(&t, sect, "platform", p->name);
    if (!strcmp(p->name, "msvc") &&
        msvc_fill(tc, or_default(&t, sect, "arch", arch_msvc),
                  or_default(&t, sect, "toolset", NULL), err, errsz) != 0) {
        toml_free(&t);
        return -1;
    }

    add_flags(&t, sect, "cflags", &tc->cflags, &tc->ncflags);
    add_flags(&t, sect, "ldflags", &tc->ldflags, &tc->nldflags);
    toml_free(&t);
    if (strcmp(p->name, "msvc") != 0 && maybe_load_msvc_env(tc, arch_msvc, err, errsz) != 0) {
        toml_free(&t);
        return -1;
    }

overlay:
    if (prefix && prefix[0]) {
        char  buf[512];
        char *n;
        free(tc->binext);
        tc->binext = sys_dup("");
        free(tc->dllpre);
        tc->dllpre = sys_dup("lib");
        n          = apply_prefix(prefix, tc->cc);
        free(tc->cc);
        tc->cc = n;
        n      = apply_prefix(prefix, tc->cxx);
        free(tc->cxx);
        tc->cxx = n;
        n       = apply_prefix(prefix, tc->ar);
        free(tc->ar);
        tc->ar = n;
        n      = apply_prefix(prefix, tc->ld);
        free(tc->ld);
        tc->ld = n;
        snprintf(buf, sizeof(buf), "cross-%s", prefix);
        size_t l = strlen(buf);
        if (l && (buf[l - 1] == '-' || buf[l - 1] == '_')) buf[l - 1] = 0;

        free(tc->platform);
        tc->platform = sys_dup(buf);
        if (sysroot && sysroot[0]) {
            add_flag(&tc->cflags, &tc->ncflags, "--sysroot");
            add_flag(&tc->cflags, &tc->ncflags, sysroot);
        }
    }

    if (extra_cflags && extra_cflags[0]) {
        char *copy = sys_dup(extra_cflags);
        char *save = NULL;
        char *tok;
        for (tok = sys_tok(copy, " \t", &save); tok; tok = sys_tok(NULL, " \t", &save)) {
            add_flag(&tc->cflags, &tc->ncflags, tok);
            add_arch_ldflags(tc, tok);
        }
        free(copy);
    }

    return 0;
}

int tc_load_star(TOOLCHAIN *tc, const char *name, const char *based, const char *cc,
                 const char *family, const char *prefix, const char *sysroot,
                 const char *extra_cflags, const char *arch, char *err, size_t errsz) {
    memset(tc, 0, sizeof(*tc));
    const TC_PRESET *p = preset_of(based && based[0] ? based : NULL);
    if (!p && family && family[0]) p = preset_of(family);
    if (!p) p = preset_of(name);
    if (!p) p = preset_of("gcc");

    const char *base = p->name;
    if (!p) {
        snprintf(err, errsz, "unknown toolchain '%s'", base);
        return -1;
    }

    load_preset(tc, p);
    free(tc->name);
    free(tc->family);
    free(tc->cc);
    free(tc->cxx);
    free(tc->ar);
    free(tc->ld);
    tc->name   = sys_dup(name);
    tc->family = sys_dup(family && family[0] ? family : p->family);
    char pfx2[512];
    snprintf(pfx2, sizeof(pfx2), "%s", p->cc);
    tc->cc  = cc && cc[0] ? sys_dup(cc) : sys_dup(p->cc);
    tc->cxx = cc && cc[0] ? apply_prefix("", cc) : sys_dup(p->cxx);
    tc->ar  = sys_dup(p->ar);
    tc->ld  = sys_dup(tc->cc);
    if (cc && cc[0] && p->cxx[0]) {
        free(tc->cxx);
        const char *dash = strrchr(cc, '-');
        char        base2[512];
        if (dash && strstr(cc, "gcc")) {
            size_t n = (size_t)(dash - cc);
            snprintf(base2, sizeof(base2), "%.*s-g++", (int)n, cc);
            tc->cxx = sys_dup(base2);
        } else {
            tc->cxx = sys_dup(p->cxx);
        }
    }

    if (prefix && prefix[0]) {
        char *n;
        n = apply_prefix(prefix, tc->cc);
        free(tc->cc);
        tc->cc = n;
        n      = apply_prefix(prefix, tc->cxx);
        free(tc->cxx);
        tc->cxx = n;
        n       = apply_prefix(prefix, tc->ar);
        free(tc->ar);
        tc->ar = n;
        n      = apply_prefix(prefix, tc->ld);
        free(tc->ld);
        tc->ld = n;
        free(tc->platform);
        tc->platform = sys_dup(p->name);
        if (sysroot && sysroot[0]) {
            add_flag(&tc->cflags, &tc->ncflags, "--sysroot");
            add_flag(&tc->cflags, &tc->ncflags, sysroot);
        }
    }

    if (extra_cflags && extra_cflags[0]) {
        char *copy = sys_dup(extra_cflags);
        char *save = NULL;
        char *tok;
        for (tok = sys_tok(copy, " \t", &save); tok; tok = sys_tok(NULL, " \t", &save)) {
            add_flag(&tc->cflags, &tc->ncflags, tok);
            add_arch_ldflags(tc, tok);
        }
        free(copy);
    }

    (void)pfx2;
    if (tc->family && !strcmp(tc->family, "msvc"))
        return msvc_fill(tc, msvc_arch(arch), NULL, err, errsz);

    return maybe_load_msvc_env(tc, msvc_arch(arch), err, errsz);
}

const char *tc_tool(const TOOLCHAIN *tc, const char *name) {
    if (!strcmp(name, "cc")) return tc->cc;
    if (!strcmp(name, "c++")) return tc->cxx;
    if (!strcmp(name, "nasm")) return tc->as;
    if (!strcmp(name, "ar")) return tc->ar;
    if (!strcmp(name, "ld")) return tc->ld;

    return name;
}

int tc_tool_ok(const TOOLCHAIN *tc, const char *tool) {
    (void)tc;
    if (!tool || !tool[0]) return 0;

    SYS_STAT st;
    if (sys_stat(tool, &st) == 0) return 1;

    if (strchr(tool, '/') || strchr(tool, '\\')) return 0;

    const char *path = getenv("PATH");
    char       *copy = path ? sys_dup(path) : NULL;
    char       *save = NULL;
    int         ok   = 0;
    for (char *d = sys_tok(copy, ":;", &save); d && !ok; d = sys_tok(NULL, ":;", &save))
        ok = exec_try(d, tool);
    free(copy);
    return ok;
}

void tc_free(TOOLCHAIN *tc) {
    char *strs[] = {tc->name,   tc->family, tc->cc,     tc->cxx,     tc->as,
                    tc->ar,     tc->ld,     tc->objext, tc->binext,  tc->libext,
                    tc->dllpre, tc->dllext, tc->soflag, tc->platform};
    for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) free(strs[i]);

    for (int i = 0; i < tc->ncflags; i++) free(tc->cflags[i]);
    for (int i = 0; i < tc->nldflags; i++) free(tc->ldflags[i]);
    for (int i = 0; i < tc->nenv; i++) free(tc->env[i]);

    free(tc->cflags);
    free(tc->ldflags);
    free(tc->env);
    memset(tc, 0, sizeof(*tc));
}
