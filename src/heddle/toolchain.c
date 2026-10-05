#include "toolchain.h"
#include "toml.h"
#include "sys.h"

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
    { "host",  "cc",    "c++",     "nasm", "ar",  ".o",   "",     ".a",   "lib", ".so",    "-shared"     },
    { "linux", "cc",    "c++",     "nasm", "ar",  ".o",   "",     ".a",   "lib", ".so",    "-shared"     },
    { "gcc",   "gcc",   "g++",     "nasm", "ar",  ".o",   "",     ".a",   "lib", ".so",    "-shared"     },
    { "clang", "clang", "clang++", "nasm", "ar",  ".o",   "",     ".a",   "lib", ".so",    "-shared"     },
    { "macos", "cc",    "c++",     "nasm", "ar",  ".o",   "",     ".a",   "lib", ".dylib", "-dynamiclib" },
    { "mingw", "x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-g++",
               "x86_64-w64-mingw32-nasm", "x86_64-w64-mingw32-ar",
               ".obj", ".exe", ".a", "lib", ".dll", "-shared" },
    { "msvc",  "cl",    "cl",      "nasm", "lib", ".obj", ".exe", ".lib", "", ".dll", "-shared" },
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

static const TC_PRESET *preset_of(const char *name) {
    for (int i = 0; i < (int)(sizeof(g_presets) / sizeof(g_presets[0])); i++)
        if (!strcmp(g_presets[i].name, name)) return &g_presets[i];

    return NULL;
}

int tc_probe(const char *preset) {
    const TC_PRESET *p = preset_of(preset);

    return p && on_path(p->cc);
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

static int apply_auto(TOOLCHAIN *tc, const char *dir, const char *user) {
    int n = (int)(sizeof(g_auto) / sizeof(g_auto[0]));

    for (int i = 0; i < n; i++) {
        const TC_PRESET *p = preset_of(g_auto[i]);

        if (!p || !on_path(p->cc)) continue;


        tc->name     = sys_dup(p->name);
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

        read_flags(dir, tc, user);
        return 0;
    }

    return -1;
}

int tc_load(TOOLCHAIN *tc, const char *dir, const char *name,
            char *err, size_t errsz) {
    memset(tc, 0, sizeof(*tc));

    if (is_auto(name)) {
        if (apply_auto(tc, dir, "host") == 0) return 0;

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

void tc_free(TOOLCHAIN *tc) {
    char *strs[] = { tc->name, tc->cc, tc->cxx, tc->as, tc->ar, tc->ld, tc->objext,
                     tc->binext, tc->libext, tc->dllpre, tc->dllext,
                     tc->soflag, tc->platform };

    for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++)
        free(strs[i]);

    for (int i = 0; i < tc->ncflags; i++) free(tc->cflags[i]);
    for (int i = 0; i < tc->nldflags; i++) free(tc->ldflags[i]);

    free(tc->cflags);
    free(tc->ldflags);

    memset(tc, 0, sizeof(*tc));
}
