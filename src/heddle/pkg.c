#include "pkg.h"

#include "hash.h"
#include "star.h"
#include "sys.h"
#include "toml.h"
#include "vcpkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_opt(const char *s) {
    return s ? sys_dup(s) : NULL;
}

static char *path_join(const char *a, const char *b) {
    if (!a || !a[0] || !strcmp(a, ".")) return sys_dup(b);

    size_t n = strlen(a) + strlen(b) + 2;
    char  *p = (char *)malloc(n);
    if (p) snprintf(p, n, "%s/%s", a, b);

    return p;
}

static char *path_join3(const char *a, const char *b, const char *c) {
    char *ab  = path_join(a, b);
    char *abc = ab ? path_join(ab, c) : NULL;
    free(ab);
    return abc;
}

static int exists(const char *p) {
    SYS_STAT st;
    return sys_stat(p, &st) == 0;
}

static int dir_exists(const char *p) {
    return sys_isdir(p);
}

static const char *kind_dir(PKG_KIND k) {
    return k == PKG_KIND_TOOLCHAIN ? "toolchain" : "library";
}

static int list_push(PKG_LIST *l, const PKG_SPEC *s) {
    if (l->n == l->cap) {
        int       newcap = l->cap ? l->cap * 2 : 8;
        PKG_SPEC *items  = (PKG_SPEC *)realloc(l->items, sizeof(PKG_SPEC) * (size_t)newcap);
        if (!items) return -1;

        l->items = items;
        l->cap   = newcap;
    }

    l->items[l->n++] = *s;
    return 0;
}

static void spec_clear(PKG_SPEC *s) {
    free(s->name);
    free(s->version);
    free(s->target);
    free(s->hash);
    free(s->store_path);
    memset(s, 0, sizeof(*s));
}

static void list_free(PKG_LIST *l) {
    for (int i = 0; i < l->n; i++) spec_clear(&l->items[i]);

    free(l->items);
    memset(l, 0, sizeof(*l));
}

static void split_at(const char *in, char **name, char **version) {
    const char *at = strrchr(in, '@');
    if (!at || at == in) {
        *name    = sys_dup(in);
        *version = sys_dup("latest");
        return;
    }

    char nb[512];
    snprintf(nb, sizeof(nb), "%.*s", (int)(at - in), in);
    *name    = sys_dup(nb);
    *version = sys_dup(at + 1);
}

typedef struct {
    const char *arch;
    const char *prefix;
    const char *cpu;
    const char *fpu_soft;
    const char *fpu_hard;
} ARCH_TEMPLATE;

static const ARCH_TEMPLATE g_arch[] = {
    {"armv6m", "arm-none-eabi-", "-mthumb -mcpu=cortex-m0", "", "-mfpu=none"},
    {"armv7m", "arm-none-eabi-", "-mthumb -mcpu=cortex-m3", "", "-mfpu=none"},
    {"armv7em", "arm-none-eabi-", "-mthumb -mcpu=cortex-m4", "", "-mfpu=fpv4-sp-d16"},
    {"armv8m", "arm-none-eabi-", "-mthumb -mcpu=cortex-m33", "", "-mfpu=fpv5-sp-d16"},
    {"cortex-m4", "arm-none-eabi-", "-mthumb -mcpu=cortex-m4", "", "-mfpu=fpv4-sp-d16"},
    {"cortex-m7", "arm-none-eabi-", "-mthumb -mcpu=cortex-m7", "", "-mfpu=fpv5-d16"},
    {"armv7a", "arm-none-eabi-", "-mcpu=cortex-a7", "", "-mfpu=neon-vfpv4"},
    {"aarch64", "aarch64-none-elf-", "-march=armv8-a", "", ""},
    {"riscv32imac", "riscv64-unknown-elf-", "-march=rv32imac -mabi=ilp32", "", ""},
    {"riscv32", "riscv64-unknown-elf-", "-march=rv32i -mabi=ilp32", "", ""},
    {"riscv64", "riscv64-unknown-elf-", "-march=rv64imac -mabi=lp64", "", ""},
    {"xtensa-lx106", "xtensa-esp32-elf-", "-mlongcalls", "", ""},
    {"xtensa", "xtensa-esp32-elf-", "-mlongcalls", "", ""},
    {"msp430", "msp430-elf-", "-mmcu=msp430f5529", "", ""},
    {"avr", "avr-", "-mmcu=atmega328p", "", ""},
    {"powerpc", "powerpc-eabi-", "-mcpu=e500mc", "", ""},
    {"x86_64", "", "-m64", "", ""},
    {"i686", "", "-m32", "", ""},
};

static const ARCH_TEMPLATE *arch_find(const char *arch) {
    if (!arch) return NULL;

    for (size_t i = 0; i < sizeof(g_arch) / sizeof(g_arch[0]); i++)
        if (!strcmp(g_arch[i].arch, arch)) return &g_arch[i];

    return NULL;
}

int pkg_target_resolve(TARGET_PROFILE *t, char *err, size_t errsz) {
    if (!t->arch || !t->arch[0]) {
        if (!t->prefix) t->prefix = sys_dup("");
        if (!t->cpu) t->cpu = sys_dup("");
        if (!t->fpu) t->fpu = sys_dup("");
        return 0;
    }

    const ARCH_TEMPLATE *a = arch_find(t->arch);
    if (!a) {
        snprintf(err, errsz, "[target] unknown arch '%s'", t->arch);
        return -1;
    }

    if (!t->prefix) t->prefix = sys_dup(a->prefix);
    if (!t->cpu) t->cpu = sys_dup(a->cpu);

    if (!t->fpu) {
        char buf[256];
        int  has_fpu = a->fpu_soft[0] || a->fpu_hard[0];
        if (!has_fpu) buf[0] = 0;
        else if (t->float_k && !strcmp(t->float_k, "soft"))
            snprintf(buf, sizeof(buf), "-mfloat-abi=soft");
        else if (t->float_k && !strcmp(t->float_k, "hard"))
            snprintf(buf, sizeof(buf), "%s -mfloat-abi=hard", a->fpu_hard);
        else if (t->float_k && !strcmp(t->float_k, "softfp"))
            snprintf(buf, sizeof(buf), "%s -mfloat-abi=softfp", a->fpu_soft);
        else snprintf(buf, sizeof(buf), "%s", a->fpu_soft);

        t->fpu = sys_dup(buf);
    }

    return 0;
}

static PKG_SOURCE parse_source(const char *s) {
    if (!s) return PKG_SOURCE_AUTO;
    if (!strcmp(s, "source") || !strcmp(s, "src")) return PKG_SOURCE_SOURCE;
    if (!strcmp(s, "binary") || !strcmp(s, "bin")) return PKG_SOURCE_BINARY;

    return PKG_SOURCE_AUTO;
}

static void load_section(const TOML *t, const char *section, PKG_KIND kind, PKG_LIST *out) {
    const TOML_TABLE *tab = NULL;
    for (int i = 0; i < t->count; i++)
        if (!strcmp(t->tables[i].name, section)) tab = &t->tables[i];

    if (!tab) return;

    for (int i = 0; i < tab->count; i++) {
        const char *key = tab->items[i].key;
        const char *val = tab->items[i].val;
        if (strchr(key, '[')) continue;

        PKG_SPEC s;
        memset(&s, 0, sizeof(s));
        s.kind         = kind;
        const char *at = strchr(val, '@');
        if (at && at != val) {
            split_at(val, &s.name, &s.version);
        } else if (strchr(key, '@')) {
            split_at(key, &s.name, &s.version);
            if (val[0] && strcmp(val, "auto")) {
                free(s.version);
                s.version = sys_dup(val);
            }
        } else {
            s.name    = sys_dup(key);
            s.version = sys_dup(val[0] ? val : "latest");
        }

        char sub[512];
        snprintf(sub, sizeof(sub), "%s.%s", section, s.name);
        s.source        = parse_source(toml_str(t, sub, "source"));
        const char *tgt = toml_str(t, sub, "target");
        if (tgt) s.target = sys_dup(tgt);

        if (list_push(out, &s) != 0) spec_clear(&s);
    }
}

static int has_recipe(const char *dir) {
    char *cfg = path_join(dir, "package.toml");
    int   ok  = cfg && exists(cfg);
    free(cfg);
    return ok;
}

static void resolve_store(PKG_MANIFEST *m) {
    for (int pass = 0; pass < 2; pass++) {
        PKG_LIST *l = pass == 0 ? &m->tools : &m->deps;
        for (int i = 0; i < l->n; i++) {
            PKG_SPEC *s = &l->items[i];
            free(s->store_path);
            char *p       = path_join3(m->store, kind_dir(s->kind), s->name);
            s->store_path = p ? path_join(p, s->version) : NULL;
            s->recipe     = has_recipe(s->store_path);
            free(p);
        }
    }
}

int pkg_manifest_finalize(PKG_MANIFEST *m, char *err, size_t errsz) {
    if (pkg_target_resolve(&m->target, err, errsz) != 0) return -1;

    free(m->toolchain_prefix);
    free(m->sysroot);
    m->toolchain_prefix = sys_dup(m->target.prefix);
    m->sysroot          = sys_dup(m->target.sysroot ? m->target.sysroot : "");
    resolve_store(m);
    return 0;
}

void pkg_manifest_add(PKG_MANIFEST *m, PKG_KIND kind, const char *name, const char *version,
                      const char *source) {
    PKG_SPEC s;
    memset(&s, 0, sizeof(s));
    s.kind    = kind;
    s.name    = sys_dup(name);
    s.version = sys_dup(version && version[0] ? version : "latest");
    if (source && source[0]) s.source = parse_source(source);

    list_push(kind == PKG_KIND_TOOLCHAIN ? &m->tools : &m->deps, &s);
}

static int pkg_manifest_load_star(PKG_MANIFEST *m, const char *star_path, char *err, size_t errsz) {
    STAR_CFG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = sys_dup(m->root);
    if (star_run(star_path, &cfg, err, errsz) != 0) {
        star_cfg_free(&cfg);
        return -1;
    }

    m->store = cfg.store ? path_join(m->root, cfg.store) : path_join3(m->root, ".heddle", "store");
    const STAR_PLATFORM *plat = NULL;
    for (int i = 0; i < cfg.npl; i++) {
        plat = &cfg.pl[i];
        break;
    }

    m->target.arch    = dup_opt(plat && plat->arch ? plat->arch : cfg.arch);
    m->target.abi     = dup_opt(plat && plat->abi ? plat->abi : cfg.abi);
    m->target.float_k = dup_opt(plat && plat->flt ? plat->flt : cfg.flt);
    m->target.sysroot = dup_opt(plat && plat->sysroot ? plat->sysroot : cfg.sysroot);
    for (int i = 0; i < cfg.ntc; i++)
        pkg_manifest_add(m, PKG_KIND_TOOLCHAIN, cfg.tc_name[i], NULL, cfg.tc_cc[i]);
    for (int i = 0; i < cfg.ndep; i++)
        pkg_manifest_add(m, PKG_KIND_LIBRARY, cfg.dep_name[i], cfg.dep_ver[i], cfg.dep_src[i]);
    star_cfg_free(&cfg);
    return pkg_manifest_finalize(m, err, errsz);
}

int pkg_manifest_load(PKG_MANIFEST *m, const char *root, char *err, size_t errsz) {
    memset(m, 0, sizeof(*m));
    m->root = sys_dup(root ? root : ".");
    char cfg[2048];
    char star[2048];
    snprintf(cfg, sizeof(cfg), "%s/heddle.toml", m->root);
    snprintf(star, sizeof(star), "%s/heddle.star", m->root);
    m->lock_path = path_join(m->root, "heddle.lock");
    if (exists(star)) {
        m->manifest = sys_dup(star);
        return pkg_manifest_load_star(m, star, err, errsz);
    }

    m->manifest = sys_dup(cfg);
    if (!exists(cfg)) {
        snprintf(err, errsz, "cannot read %s or %s", cfg, star);
        return -1;
    }

    TOML t;
    toml_init(&t);
    if (toml_parse(&t, cfg, err, errsz) != 0) {
        toml_free(&t);
        return -1;
    }

    const char *store = toml_str(&t, "build", "store");
    m->store          = store ? path_join(m->root, store) : path_join3(m->root, ".heddle", "store");
    m->target.arch    = dup_opt(toml_str(&t, "target", "arch"));
    m->target.abi     = dup_opt(toml_str(&t, "target", "abi"));
    m->target.float_k = dup_opt(toml_str(&t, "target", "float"));
    m->target.sysroot = dup_opt(toml_str(&t, "target", "sysroot"));
    load_section(&t, "toolchain", PKG_KIND_TOOLCHAIN, &m->tools);
    load_section(&t, "dependencies", PKG_KIND_LIBRARY, &m->deps);
    toml_free(&t);
    if (pkg_target_resolve(&m->target, err, errsz) != 0) {
        pkg_manifest_free(m);
        return -1;
    }

    m->toolchain_prefix = sys_dup(m->target.prefix);
    m->sysroot          = sys_dup(m->target.sysroot ? m->target.sysroot : "");
    resolve_store(m);
    return 0;
}

void pkg_manifest_free(PKG_MANIFEST *m) {
    list_free(&m->tools);
    list_free(&m->deps);
    free(m->root);
    free(m->store);
    free(m->lock_path);
    free(m->manifest);
    free(m->toolchain_prefix);
    free(m->sysroot);
    free(m->target.arch);
    free(m->target.abi);
    free(m->target.float_k);
    free(m->target.prefix);
    free(m->target.sysroot);
    free(m->target.cpu);
    free(m->target.fpu);
    memset(m, 0, sizeof(*m));
}

typedef struct {
    char **items;
    int    n;
    int    cap;
} STRLIST;

static void strlist_free(STRLIST *l) {
    for (int i = 0; i < l->n; i++) free(l->items[i]);

    free(l->items);
    memset(l, 0, sizeof(*l));
}

static void strlist_push(STRLIST *l, char *s) {
    if (l->n == l->cap) {
        int    newcap = l->cap ? l->cap * 2 : 16;
        char **items  = (char **)realloc(l->items, sizeof(char *) * (size_t)newcap);
        if (!items) {
            free(s);
            return;
        }

        l->items = items;
        l->cap   = newcap;
    }

    l->items[l->n++] = s;
}

#if defined(_WIN32)
#include <windows.h>

static int list_dir(const char *path, STRLIST *out) {
    char pat[4096];
    snprintf(pat, sizeof(pat), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;

        char *full = path_join(path, fd.cFileName);
        if (!full) continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char *d = (char *)malloc(strlen(full) + 2);
            if (d) {
                sprintf(d, "%s\\", full);
                free(full);
                full = d;
            }
        }

        strlist_push(out, full);
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return 0;
}
#else
#include <dirent.h>

static int list_dir(const char *path, STRLIST *out) {
    DIR *d = opendir(path);
    if (!d) return -1;

    struct dirent *e;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char *full = path_join(path, e->d_name);
        if (!full) continue;

        if (sys_isdir(full)) {
            size_t n  = strlen(full) + 2;
            char  *d2 = (char *)malloc(n);
            if (d2) {
                snprintf(d2, n, "%s/", full);
                free(full);
                full = d2;
            }
        }

        strlist_push(out, full);
    }

    closedir(d);
    return 0;
}
#endif

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static const char *rel_of(const char *root, const char *full) {
    const char *r = full + strlen(root);
    while (*r == '/' || *r == '\\') r++;

    return r;
}

static int is_dir_name(const char *r) {
    size_t n = strlen(r);
    return n > 0 && (r[n - 1] == '/' || r[n - 1] == '\\');
}

static unsigned long long hash_tree_rec(const char *dir, const char *base, unsigned long long h) {
    STRLIST l;
    memset(&l, 0, sizeof(l));
    if (list_dir(dir, &l) != 0) {
        strlist_free(&l);
        return h;
    }

    qsort(l.items, (size_t)l.n, sizeof(char *), cmp_str);
    for (int i = 0; i < l.n; i++) {
        const char *name = rel_of(dir, l.items[i]);
        char        rel[4096];
        snprintf(rel, sizeof(rel), "%.2000s%.2000s", base, name);
        h = hash_text(h, rel);
        if (is_dir_name(name)) {
            char sub[4096];
            snprintf(sub, sizeof(sub), "%.4000s/", rel);
            h = hash_tree_rec(l.items[i], sub, h);
        } else {
            HASH_DB db;
            hash_db_init(&db);
            h = hash_u64(h, hash_read_file(&db, l.items[i]));
            hash_db_free(&db);
        }
    }

    strlist_free(&l);
    return h;
}

static void hash_tree(const char *dir, char *out, size_t cap) {
    unsigned long long h = hash_tree_rec(dir, "", HASH_FNV_OFFSET);
    snprintf(out, cap, "sha256:%016llx", h);
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;

    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }

    char   buf[65536];
    size_t got;
    int    rc = 0;
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, got, out) != got) {
            rc = -1;
            break;
        }

    fclose(in);
    if (fclose(out) != 0) rc = -1;

    return rc;
}

static int copy_tree(const char *src, const char *dst) {
    sys_mkpath(dst);
    STRLIST l;
    memset(&l, 0, sizeof(l));
    if (list_dir(src, &l) != 0) {
        strlist_free(&l);
        return -1;
    }

    int rc = 0;
    for (int i = 0; i < l.n && rc == 0; i++) {
        const char *rel = rel_of(src, l.items[i]);
        char       *d   = path_join(dst, rel);
        if (!d) {
            rc = -1;
            break;
        }

        rc = is_dir_name(rel) ? copy_tree(l.items[i], d) : copy_file(l.items[i], d);
        free(d);
    }

    strlist_free(&l);
    return rc;
}

static void remove_tree(const char *path) {
    STRLIST l;
    memset(&l, 0, sizeof(l));
    if (list_dir(path, &l) == 0) {
        for (int i = 0; i < l.n; i++)
            is_dir_name(l.items[i]) ? remove_tree(l.items[i]) : (void)remove(l.items[i]);
        strlist_free(&l);
    }

    remove(path);
}

static int sh(const char *fmt, const char *a, const char *b) {
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), fmt, a, b);
    return system(cmd) == 0 ? 0 : -1;
}

static int is_payload_dir(const char *r) {
    return !strcmp(r, "bin/") || !strcmp(r, "include/") || !strcmp(r, "lib/") || !strcmp(r, "src/");
}

static void hoist(const char *dst, const char *wrap) {
    STRLIST c;
    memset(&c, 0, sizeof(c));
    if (list_dir(wrap, &c) != 0) {
        strlist_free(&c);
        return;
    }

    for (int i = 0; i < c.n; i++) {
        char *d = path_join(dst, rel_of(wrap, c.items[i]));
        if (d) {
            if (rename(c.items[i], d) != 0) copy_tree(c.items[i], d);
            free(d);
        }
    }

    strlist_free(&c);
    remove_tree(wrap);
}

static int unpack(const char *archive, const char *dst) {
    sys_mkpath(dst);
    if (sh("tar -xzf '%s' -C '%s' >/dev/null 2>&1", archive, dst) != 0) return -1;

    for (int guard = 0; guard < 8; guard++) {
        STRLIST l;
        memset(&l, 0, sizeof(l));
        if (list_dir(dst, &l) != 0 || l.n != 1 || !is_dir_name(l.items[0])) {
            strlist_free(&l);
            break;
        }

        if (is_payload_dir(rel_of(dst, l.items[0]))) {
            strlist_free(&l);
            break;
        }

        char *wrap = l.items[0];
        l.n        = 0;
        hoist(dst, wrap);
        strlist_free(&l);
        free(wrap);
    }

    return 0;
}

static const char *registry(void) {
    const char *r = getenv("HEDDLE_REGISTRY");
    return r && r[0] ? r : NULL;
}

static int is_http(const char *u) {
    return !strncmp(u, "http://", 7) || !strncmp(u, "https://", 8);
}

static int fetch_pkg(const PKG_SPEC *s, const char *dst, int offline, char *err, size_t errsz) {
    const char *reg = registry();
    if (offline || !reg) {
        snprintf(err, errsz, "%s %s@%s not in store and offline%s", kind_dir(s->kind), s->name,
                 s->version, reg ? "" : " (no HEDDLE_REGISTRY)");
        return -1;
    }

    char rel[1024];
    snprintf(rel, sizeof(rel), "%s/%s/%s", kind_dir(s->kind), s->name, s->version);
    if (is_http(reg)) {
        char url[2048];
        char tmp[2048];
        snprintf(url, sizeof(url), "%s/%s.tar.gz", reg, rel);
        snprintf(tmp, sizeof(tmp), "%s.tmp.tgz", dst);
        if (sh("curl -fsS -o '%s' '%s' >/dev/null 2>&1", tmp, url) != 0) {
            snprintf(err, errsz, "cannot fetch %s", url);
            return -1;
        }

        int rc = unpack(tmp, dst);
        remove(tmp);
        if (rc != 0) snprintf(err, errsz, "cannot unpack %s", url);

        return rc;
    }

    char *dir   = path_join(reg, rel);
    char *targz = NULL;
    if (dir) {
        size_t n = strlen(dir) + 8;
        targz    = (char *)malloc(n);
        if (targz) snprintf(targz, n, "%s.tar.gz", dir);
    }

    int rc = -1;
    if (dir && dir_exists(dir)) {
        rc = copy_tree(dir, dst);
    } else if (targz && exists(targz)) {
        rc = unpack(targz, dst);
    } else if (vcpkg_is_registry(reg)) {
        char *port = path_join3(reg, "ports", s->name);
        if (port && dir_exists(port))
            snprintf(err, errsz,
                     "vcpkg port '%s' needs a built tree; run "
                     "'heddle vcpkg import' first",
                     s->name);
        else snprintf(err, errsz, "vcpkg registry has no port '%s'", s->name);

        free(port);
    } else {
        snprintf(err, errsz, "registry has no %s", rel);
    }

    free(dir);
    free(targz);
    return rc;
}

static const char *lock_lookup(const char *path, const char *kind, const char *name,
                               const char *version) {
    static char buf[256];
    FILE       *f = fopen(path, "r");
    if (!f) return NULL;

    char  line[1024];
    char *found = NULL;
    while (fgets(line, sizeof(line), f)) {
        char k[64], n[256], v[128], h[256];
        if (line[0] == '#') continue;
        if (sscanf(line, "%63s %255s %127s %255s", k, n, v, h) != 4) continue;
        if (strcmp(k, kind) || strcmp(n, name) || strcmp(v, version)) continue;

        snprintf(buf, sizeof(buf), "%s", h);
        found = buf;
        break;
    }

    fclose(f);
    return found;
}

static int lock_write(const PKG_MANIFEST *m, char *err, size_t errsz) {
    FILE *f = fopen(m->lock_path, "w");
    if (!f) {
        snprintf(err, errsz, "cannot write %s", m->lock_path);
        return -1;
    }

    fputs("# generated by heddle; do not edit\n", f);
    for (int pass = 0; pass < 2; pass++) {
        const PKG_LIST *l = pass == 0 ? &m->tools : &m->deps;
        const char     *k = pass == 0 ? "toolchain" : "library";
        for (int i = 0; i < l->n; i++)
            if (l->items[i].hash)
                fprintf(f, "%s %s %s %s\n", k, l->items[i].name, l->items[i].version,
                        l->items[i].hash);
    }

    fclose(f);
    return 0;
}

static void chmod_bin(const char *dir) {
    char *bin = path_join(dir, "bin");
    if (bin && dir_exists(bin)) {
        STRLIST l;
        memset(&l, 0, sizeof(l));
        if (list_dir(bin, &l) == 0)
            for (int i = 0; i < l.n; i++)
                if (!is_dir_name(l.items[i])) sys_chmod(l.items[i], 0755);

        strlist_free(&l);
    }

    free(bin);
}

static int install_list(PKG_MANIFEST *m, PKG_LIST *l, int offline, int verbose, char *err,
                        size_t errsz) {
    for (int i = 0; i < l->n; i++) {
        PKG_SPEC *s   = &l->items[i];
        char     *dst = sys_dup(s->store_path);
        if (!dst) {
            snprintf(err, errsz, "out of memory");
            return -1;
        }

        char        hex[64];
        const char *want  = lock_lookup(m->lock_path, kind_dir(s->kind), s->name, s->version);
        int         drift = 0;
        if (dir_exists(dst)) {
            hash_tree(dst, hex, sizeof(hex));
            drift = want && strcmp(want, hex);
        }

        if (!dir_exists(dst) || drift) {
            if (drift && offline) {
                snprintf(err, errsz, "%s %s@%s drifted and --offline forbids repair",
                         kind_dir(s->kind), s->name, s->version);
                free(dst);
                return -1;
            }

            if (dir_exists(dst)) remove_tree(dst);

            if (verbose)
                fprintf(stderr, "heddle: %s %s %s@%s\n", drift ? "repairing" : "fetching",
                        kind_dir(s->kind), s->name, s->version);
            if (fetch_pkg(s, dst, offline, err, errsz) != 0) {
                free(dst);
                return -1;
            }
        }

        free(s->store_path);
        s->store_path = sys_dup(dst);
        if (s->kind == PKG_KIND_TOOLCHAIN) chmod_bin(dst);

        hash_tree(dst, hex, sizeof(hex));
        if (want && strcmp(want, hex))
            fprintf(stderr,
                    "heddle: warning: %s %s@%s content changed "
                    "(lock %s, store %s)\n",
                    kind_dir(s->kind), s->name, s->version, want, hex);
        free(s->hash);
        s->hash = sys_dup(hex);
        if (verbose) fprintf(stderr, "heddle:   store %s (%s)\n", dst, hex);

        free(dst);
    }

    return 0;
}

int pkg_install(PKG_MANIFEST *m, int offline, int verbose, char *err, size_t errsz) {
    if (!m->tools.n && !m->deps.n) {
        if (verbose) fprintf(stderr, "heddle: nothing to install\n");
        return 0;
    }

    sys_mkpath(m->store);
    if (verbose) fprintf(stderr, "heddle: store %s\n", m->store);

    if (install_list(m, &m->tools, offline, verbose, err, errsz) != 0) return -1;

    if (install_list(m, &m->deps, offline, verbose, err, errsz) != 0) return -1;

    return lock_write(m, err, errsz);
}

int pkg_verify(PKG_MANIFEST *m, int verbose, char *err, size_t errsz) {
    if (!exists(m->lock_path)) {
        snprintf(err, errsz, "no lockfile at %s; run 'heddle tool install'", m->lock_path);
        return -1;
    }

    int bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        PKG_LIST *l = pass == 0 ? &m->tools : &m->deps;
        for (int i = 0; i < l->n; i++) {
            PKG_SPEC *s = &l->items[i];
            if (!dir_exists(s->store_path)) {
                fprintf(stderr, "heddle: %s %s@%s: not installed\n", kind_dir(s->kind), s->name,
                        s->version);
                bad++;
                continue;
            }

            char hex[64];
            hash_tree(s->store_path, hex, sizeof(hex));
            const char *want = lock_lookup(m->lock_path, kind_dir(s->kind), s->name, s->version);
            if (!want) {
                fprintf(stderr, "heddle: %s %s@%s: missing from lockfile\n", kind_dir(s->kind),
                        s->name, s->version);
                bad++;
            } else if (strcmp(want, hex)) {
                fprintf(stderr, "heddle: %s %s@%s: drift (lock %s, store %s)\n", kind_dir(s->kind),
                        s->name, s->version, want, hex);
                bad++;
            } else if (verbose) {
                fprintf(stderr, "heddle: ok %s %s@%s\n", kind_dir(s->kind), s->name, s->version);
            }
        }
    }

    if (bad) {
        snprintf(err, errsz, "environment does not match %s", m->lock_path);
        return -1;
    }

    return 0;
}

static const char *source_name(PKG_SOURCE s) {
    if (s == PKG_SOURCE_SOURCE) return "source";
    if (s == PKG_SOURCE_BINARY) return "binary";

    return "auto";
}

void pkg_plan(const PKG_MANIFEST *m) {
    printf("heddle: plan\n");
    if (m->target.arch) {
        printf("  architecture: %s", m->target.arch);
        if (m->target.abi) printf(" abi=%s", m->target.abi);
        if (m->target.float_k) printf(" float=%s", m->target.float_k);
        printf("\n  prefix:       %s\n  cflags:      %s %s\n", m->target.prefix, m->target.cpu,
               m->target.fpu);
    }

    for (int pass = 0; pass < 2; pass++) {
        const PKG_LIST *l = pass == 0 ? &m->tools : &m->deps;
        for (int i = 0; i < l->n; i++) {
            const PKG_SPEC *s = &l->items[i];
            printf("  %-10s %-16s %-10s %s\n", kind_dir(s->kind), s->name, s->version,
                   source_name(s->source));
        }
    }
}

typedef struct {
    char  *p;
    size_t n;
    size_t cap;
} SBUF;

static void sbuf_add(SBUF *s, const char *fmt, ...) {
    if (!s->p) {
        s->cap = 256;
        s->p   = (char *)malloc(s->cap);
        if (!s->p) return;
        s->p[0] = 0;
    }

    for (;;) {
        va_list ap, ap2;
        va_start(ap, fmt);
        va_copy(ap2, ap);

        size_t avail = s->cap - s->n;
        int    need  = vsnprintf(s->p + s->n, avail, fmt, ap);
        va_end(ap);

        if (need < 0) {
            va_end(ap2);
            return;
        }

        if ((size_t)need < avail) {
            s->n += (size_t)need;
            va_end(ap2);
            return;
        }

        size_t newcap = s->n + (size_t)need + 1;
        char  *np     = (char *)realloc(s->p, newcap);

        if (!np) {
            va_end(ap2);
            return;
        }

        s->p   = np;
        s->cap = newcap;

        va_end(ap2);
    }
}

static char *env_kv(const char *key, const char *val) {
    size_t n   = strlen(key) + strlen(val) + 2;
    char  *out = (char *)malloc(n);

    if (out) snprintf(out, n, "%s=%s", key, val);

    return out;
}

static char *sbuf_take(SBUF *s) {
    if (!s->p) return sys_dup("");

    char *out = s->p;

    s->p = NULL;
    s->n = s->cap = 0;
    return out;
}

static char *tool_path(const PKG_MANIFEST *m) {
    SBUF s = {0};

    for (int i = 0; i < m->tools.n; i++) {
        char *bin = path_join(m->tools.items[i].store_path, "bin");

        if (bin && dir_exists(bin)) sbuf_add(&s, "%s%s", s.n ? ":" : "", bin);

        free(bin);
    }

    const char *sys = getenv("PATH");

    sbuf_add(&s, "%s%s", s.n ? ":" : "", sys ? sys : "");

    return sbuf_take(&s);
}

char **pkg_env(const PKG_MANIFEST *m, int *out_n) {
    *out_n = 0;

    SBUF cflags = {0}, ldflags = {0}, cpath = {0};

    sbuf_add(&cflags, "%s %s", m->target.cpu ? m->target.cpu : "",
             m->target.fpu ? m->target.fpu : "");

    for (int i = 0; i < m->deps.n; i++) {
        const PKG_SPEC *s   = &m->deps.items[i];
        char           *inc = path_join(s->store_path, "include");
        char           *lib = path_join(s->store_path, "lib");

        if (inc && dir_exists(inc)) {
            sbuf_add(&cflags, " -I%s", inc);
            sbuf_add(&cpath, "%s%s", cpath.n ? ":" : "", inc);
        }

        if (lib && dir_exists(lib)) {
            sbuf_add(&ldflags, " -L%s", lib);
            sbuf_add(&ldflags, " -l%s", s->name);
        }

        free(inc);
        free(lib);
    }

    char **v = (char **)calloc(5, sizeof(char *));
    if (!v) return NULL;

    char *pv = tool_path(m);
    char *cf = sbuf_take(&cflags);
    char *ld = sbuf_take(&ldflags);
    char *cp = sbuf_take(&cpath);

    int n = 0;
    v[n++] = env_kv("PATH", pv);
    v[n++] = env_kv("CFLAGS", cf);
    v[n++] = env_kv("CXXFLAGS", cf);
    v[n++] = env_kv("LDFLAGS", ld);

    if (cp[0]) v[n++] = env_kv("CPATH", cp);

    free(pv);
    free(cf);
    free(ld);
    free(cp);

    *out_n = n;
    return v;
}

int pkg_prepend_path(PKG_MANIFEST *m) {
    if (m->tools.n == 0) return 0;

    char *full = tool_path(m);
    if (!full) return -1;

#if defined(_WIN32)
    _putenv_s("PATH", full);
#else
    setenv("PATH", full, 1);
#endif

    free(full);
    return 0;
}

char *pkg_variant_key(const TARGET_PROFILE *t, const TOOLCHAIN *tc, char *out, size_t cap) {
    unsigned long long h       = HASH_FNV_OFFSET;
    const char        *parts[] = {
        t->arch, t->abi, t->float_k, t->cpu, t->fpu, tc->cc, tc->platform,
    };

    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        if (parts[i]) h = hash_text(h, parts[i]);

    for (int i = 0; i < tc->ncflags; i++) h = hash_text(h, tc->cflags[i]);

    snprintf(out, cap, "%s-%s-%016llx", t->arch ? t->arch : "host", tc->name ? tc->name : "cc", h);
    return out;
}

void pkg_hash_tree(const char *dir, char *out, size_t cap) {
    hash_tree(dir, out, cap);
}

int pkg_copy_tree(const char *src, const char *dst) {
    return copy_tree(src, dst);
}

int pkg_lock_add(const char *lock, const char *kind, const char *name, const char *version,
                 const char *hash, char *err, size_t errsz) {
    FILE *in  = fopen(lock, "r");
    FILE *out = NULL;
    char  tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", lock);
    out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        snprintf(err, errsz, "cannot write %s", tmp);
        return -1;
    }

    fputs("# generated by heddle; do not edit\n", out);
    char line[1024];
    int  written = 0;
    while (in && fgets(line, sizeof(line), in)) {
        if (line[0] == '#') continue;

        char k[64], n[256], v[128], h[256];
        if (sscanf(line, "%63s %255s %127s %255s", k, n, v, h) != 4) continue;

        if (!strcmp(k, kind) && !strcmp(n, name)) {
            fprintf(out, "%s %s %s %s\n", kind, name, version, hash);
            written = 1;
            continue;
        }

        fprintf(out, "%s %s %s %s\n", k, n, v, h);
    }

    if (in) fclose(in);

    if (!written) fprintf(out, "%s %s %s %s\n", kind, name, version, hash);

    fclose(out);
    if (rename(tmp, lock) != 0) {
        snprintf(err, errsz, "cannot update %s", lock);
        return -1;
    }

    return 0;
}
