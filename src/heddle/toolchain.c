#include "toolchain.h"
#include "sys.h"
#include "toml.h"
#include "vswhere.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
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
    { "host",  "gnu",  "cc",    "c++",     "nasm", "ar",  ".o",   "",     ".a",   "lib", ".so",    "-shared"     },
    { "linux", "gnu",  "cc",    "c++",     "nasm", "ar",  ".o",   "",     ".a",   "lib", ".so",    "-shared"     },
    { "gcc",   "gnu",  "gcc",   "g++",     "nasm", "ar",  ".o",   "",     ".a",   "lib", ".so",    "-shared"     },
    { "clang", "gnu",  "clang", "clang++", "nasm", "ar",  ".o",   "",     ".a",   "lib", ".so",    "-shared"     },
    { "macos", "gnu",  "cc",    "c++",     "nasm", "ar",  ".o",   "",     ".a",   "lib", ".dylib", "-dynamiclib" },
    { "armcc", "armcc","armcc", "armcc",   "armasm", "armar", ".o", "",   ".lib", "",    ".dll",   ""            },
    { "iar",   "iar",  "iccarm","iccarm",  "iasm",  "iarchive", ".o", "", ".a",   "",    ".dll",   ""            },
    { "mingw", "gnu",  "x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-g++",
               "x86_64-w64-mingw32-nasm", "x86_64-w64-mingw32-ar",
               ".obj", ".exe", ".a", "lib", ".dll", "-shared" },
    { "msvc",  "msvc", "cl",    "cl",      "nasm", "lib", ".obj", ".exe", ".lib", "", ".dll", "-shared" },
};

static const char *const g_auto[] = { "gcc", "clang", "tcc", "msvc" };

static int on_path(const char *prog) {
    if (strchr(prog, '/') || strchr(prog, '\\'))
        return access(prog, X_OK) == 0;

    const char *path = getenv("PATH");
    if (!path) return 0;

    char *copy = sys_dup(path);
    if (!copy) return 0;

    int   found = 0;
    char *dir  = copy;

    while (dir && *dir && !found) {
        char  *sep = strchr(dir, ':');
        size_t len = sep ? (size_t)(sep - dir) : strlen(dir);
        char   full[1024];

        snprintf(full, sizeof(full), "%.*s%s%s", (int)len, dir, len ? "/" : "", prog);
        found = EXEC_OK(full);

        if (!sep) break;

        dir = sep + 1;
    }

    free(copy);
    return found;
}

void tc_add_env(TOOLCHAIN *tc, const char *fmt, ...) {
    char    buf[4096];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char **next = (char **)realloc(tc->env, sizeof(char *) * (size_t)(tc->nenv + 1));
    if (!next) return;

    tc->env = next;
    tc->env[tc->nenv] = sys_dup(buf);

    if (tc->env[tc->nenv]) tc->nenv++;
}

static const TC_PRESET *preset_of(const char *name) {
    for (int i = 0; i < (int)(sizeof(g_presets) / sizeof(g_presets[0])); i++)
        if (!strcmp(g_presets[i].name, name)) return &g_presets[i];

    return NULL;
}

static int msvc_fill(TOOLCHAIN *tc, const char *arch, const char *want) {
    char install[1024];
    char toolset[1024];
    char inc[2048];
    char lib[2048];
    char cl[1200];

    if (vs_install(install, sizeof(install)) != 0) return -1;
    if (vs_toolset(install, want, toolset, sizeof(toolset)) != 0) return -1;

    if (vs_sdk(arch, inc, sizeof(inc), lib, sizeof(lib)) != 0)
        inc[0] = lib[0] = 0;

    snprintf(cl, sizeof(cl), "%s\\bin\\Host%s\\%s\\cl.exe",
             toolset, arch, arch);

    free(tc->cc);
    free(tc->cxx);
    free(tc->ar);
    free(tc->ld);

    tc->cc  = sys_dup(cl);
    tc->cxx = sys_dup(cl);
    tc->ld  = sys_dup(cl);
    tc->ar  = sys_dup("lib");

    char *sys_path = getenv("PATH");

    tc_add_env(tc, "PATH=%s\\bin\\Host%s\\%s;%s",
               toolset, arch, arch, sys_path ? sys_path : "");
    tc_add_env(tc, "INCLUDE=%s;%s\\include", inc, toolset);
    tc_add_env(tc, "LIB=%s;%s\\lib\\%s", lib, toolset, arch);

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

int tc_auto_count(void) {
    return (int)(sizeof(g_auto) / sizeof(g_auto[0]));
}

const char *tc_auto_at(int i) {
    if (i < 0 || i >= tc_auto_count()) return NULL;

    return g_auto[i];
}

static const char *or_default(const TOML *t, const char *sect,
                              const char *key, const char *fallback) {
    const char *v = toml_str(t, sect, key);

    return v ? v : fallback;
}

static char *dup_or(const TOML *t, const char *sect,
                    const char *key, const char *fallback) {
    return sys_dup(or_default(t, sect, key, fallback));
}

static void add_flag(char ***arr, int *n, const char *v) {
    char **next = (char **)realloc(*arr, sizeof(char *) * (size_t)(*n + 1));
    if (!next) return;

    *arr = next;
    (*arr)[*n] = sys_dup(v);
    (*n)++;
}

static void add_flags(TOML *t, const char *sect, const char *key,
                      char ***arr, int *n) {
    for (int i = 0; ; i++) {
        const char *v = toml_arr(t, sect, key, i);

        if (!v) break;

        add_flag(arr, n, v);
    }
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

static void load_preset(TOOLCHAIN *tc, const TC_PRESET *p) {
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
}

static int apply_auto(TOOLCHAIN *tc, const char *dir, const char *user) {
    for (int i = 0; i < tc_auto_count(); i++) {
        const TC_PRESET *p = preset_of(g_auto[i]);

        if (!p || !tc_probe(g_auto[i])) continue;

        load_preset(tc, p);

        if (!strcmp(p->name, "msvc")) {
            free(tc->cc);
            tc->cc = NULL;

            if (msvc_fill(tc, "x64", NULL) != 0) continue;
        }

        read_flags(dir, tc, user);
        return 0;
    }

    return -1;
}

int tc_load(TOOLCHAIN *tc, const char *dir, const char *name,
            char *err, size_t errsz) {
    return tc_load_ex(tc, dir, name, NULL, NULL, NULL, err, errsz);
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

int tc_load_ex(TOOLCHAIN *tc, const char *dir, const char *name,
               const char *prefix, const char *sysroot,
               const char *extra_cflags,
               char *err, size_t errsz) {
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

        if (apply_auto(tc, dir, "host") == 0) goto overlay;

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

    if (!strcmp(p->name, "msvc"))
        msvc_fill(tc, or_default(&t, sect, "arch", "x64"),
                  or_default(&t, sect, "toolset", NULL));

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

    add_flags(&t, sect, "cflags", &tc->cflags, &tc->ncflags);
    add_flags(&t, sect, "ldflags", &tc->ldflags, &tc->nldflags);

    toml_free(&t);

overlay:
    if (prefix && prefix[0]) {
        char buf[512];

        char *n;

        n = apply_prefix(prefix, tc->cc);  free(tc->cc);  tc->cc  = n;
        n = apply_prefix(prefix, tc->cxx); free(tc->cxx); tc->cxx = n;
        n = apply_prefix(prefix, tc->ar);  free(tc->ar);  tc->ar  = n;
        n = apply_prefix(prefix, tc->ld);  free(tc->ld);  tc->ld  = n;

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

        for (tok = sys_tok(copy, " \t", &save); tok;
             tok = sys_tok(NULL, " \t", &save))
            add_flag(&tc->cflags, &tc->ncflags, tok);

        free(copy);
    }

    return 0;
}

const char *tc_tool(const TOOLCHAIN *tc, const char *name) {
    if (!strcmp(name, "cc"))  return tc->cc;
    if (!strcmp(name, "c++")) return tc->cxx;
    if (!strcmp(name, "nasm")) return tc->as;
    if (!strcmp(name, "ar"))  return tc->ar;
    if (!strcmp(name, "ld"))  return tc->ld;

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

    for (char *d = sys_tok(copy, ":", &save); d && !ok;
         d = sys_tok(NULL, ":", &save)) {
        char buf[2048];

        snprintf(buf, sizeof(buf), "%s/%s", d, tool);
        ok = sys_stat(buf, &st) == 0;
    }

    free(copy);
    return ok;
}

void tc_free(TOOLCHAIN *tc) {
    char *strs[] = { tc->name, tc->family, tc->cc, tc->cxx, tc->as, tc->ar, tc->ld, tc->objext,
                     tc->binext, tc->libext, tc->dllpre, tc->dllext,
                     tc->soflag, tc->platform };

    for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++)
        free(strs[i]);

    for (int i = 0; i < tc->ncflags; i++) free(tc->cflags[i]);
    for (int i = 0; i < tc->nldflags; i++) free(tc->ldflags[i]);
    for (int i = 0; i < tc->nenv; i++) free(tc->env[i]);

    free(tc->cflags);
    free(tc->ldflags);
    free(tc->env);

    memset(tc, 0, sizeof(*tc));
}
