
#include "project.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PKG_MAX_DEPTH 8

static int path_exists(const char *p) {
    SYS_STAT st;
    return sys_stat(p, &st) == 0;
}

static int dir_exists(const char *p) {
    return sys_isdir(p);
}

static char *str_join(const char *a, const char *b) {
    if (!a || !a[0] || !strcmp(a, ".")) return sys_dup(b);

    size_t n = strlen(a) + strlen(b) + 2;
    char  *p = (char *)malloc(n);

    if (p) snprintf(p, n, "%s/%s", a, b);

    return p;
}

static int vec_add(char ***v, int *n, const char *s) {
    char **next = (char **)realloc(*v, sizeof(char *) * (size_t)(*n + 1));
    if (!next) return -1;

    *v = next;
    (*v)[*n] = sys_dup(s);

    if (!(*v)[*n]) return -1;

    (*n)++;
    return 0;
}

static void vec_free(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);

    free(v);
}

static TARGET *target_add(PROJECT *p, const char *name, const char *dir) {
    if (p->ntargets == p->cap) {
        int newcap = p->cap ? p->cap * 2 : 16;

        TARGET *targets = (TARGET *)realloc(
            p->targets, sizeof(TARGET) * (size_t)newcap);
        if (!targets) return NULL;

        p->targets = targets;
        p->cap     = newcap;
    }

    TARGET *t = &p->targets[p->ntargets++];
    memset(t, 0, sizeof(*t));

    t->name = sys_dup(name);
    t->dir  = sys_dup(dir ? dir : "");
    return t;
}

static void target_free(TARGET *t) {
    free(t->name);
    free(t->dir);

    vec_free(t->src, t->nsrc);
    vec_free(t->inc, t->ninc);
    vec_free(t->deps, t->ndeps);
    vec_free(t->cflags, t->ncflags);
    vec_free(t->ldflags, t->nldflags);

    memset(t, 0, sizeof(*t));
}

static int parse_type(const char *s, TARGET_TYPE *out) {
    if (!strcmp(s, "exe") || !strcmp(s, "executable") || !strcmp(s, "bin")) {
        *out = TARGET_EXE;
        return 0;
    }

    if (!strcmp(s, "staticlib") || !strcmp(s, "lib")) {
        *out = TARGET_STATICLIB;
        return 0;
    }

    if (!strcmp(s, "sharedlib") || !strcmp(s, "dylib") ||
        !strcmp(s, "so")) {
        *out = TARGET_SHAREDLIB;
        return 0;
    }

    return -1;
}

static char *prefixed(const char *dir, const char *rel) {
    if (rel[0] == '/') return sys_dup(rel);
    if (!dir || !dir[0]) return sys_dup(rel);

    return str_join(dir, rel);
}

static int load_strings(const TOML *cfg, const char *section, const char *key,
                        const char *dir, char ***out, int *n) {
    for (int i = 0; ; i++) {
        const char *v = toml_arr(cfg, section, key, i);

        if (!v) return 0;

        char *full = dir ? prefixed(dir, v) : sys_dup(v);

        if (!full) return -1;

        vec_add(out, n, full);
        free(full);
    }
}

static int target_fill(PROJECT *p, TARGET *t, const TOML *cfg,
                       const char *section, char *err, size_t errsz) {
    const char *type = toml_str(cfg, section, "type");

    if (!type) {
        snprintf(err, errsz, "section [%s]: missing key 'type'", section);
        return -1;
    }

    if (parse_type(type, &t->type) != 0) {
        snprintf(err, errsz, "section [%s]: unknown type '%s'", section, type);
        return -1;
    }

    if (load_strings(cfg, section, "src", t->dir, &t->src, &t->nsrc) != 0)
        return -1;

    if (load_strings(cfg, section, "inc", t->dir, &t->inc, &t->ninc) != 0)
        return -1;

    if (load_strings(cfg, section, "deps", NULL, &t->deps, &t->ndeps) != 0)
        return -1;

    if (load_strings(cfg, section, "cflags", NULL, &t->cflags, &t->ncflags) != 0)
        return -1;

    if (load_strings(cfg, section, "ldflags", NULL, &t->ldflags, &t->nldflags) != 0)
        return -1;

    if (t->nsrc == 0) {
        snprintf(err, errsz, "section [%s]: key 'src' is empty", section);
        return -1;
    }

    (void)p;
    return 0;
}

static const char *norm_dir(const char *d) {
    while (d[0] == '.' && d[1] == '/') d += 2;

    return d;
}

static int load_package(PROJECT *p, const char *pkgdir, int depth,
                        char *err, size_t errsz) {
    if (depth >= PKG_MAX_DEPTH) {
        snprintf(err, errsz, "package nesting deeper than %d", PKG_MAX_DEPTH);
        return -1;
    }

    char cfg[2048];
    snprintf(cfg, sizeof(cfg), "%s/heddle.toml", pkgdir);

    if (!path_exists(cfg)) {
        snprintf(err, errsz, "cannot read %s", cfg);
        return -1;
    }

    TOML local;
    toml_init(&local);

    if (toml_parse(&local, cfg, err, errsz) != 0) {
        toml_free(&local);
        return -1;
    }

    char **names = NULL;
    int    n = toml_sections(&local, "target.", &names);

    for (int i = 0; i < n; i++) {
        char section[512];
        snprintf(section, sizeof(section), "target.%s", names[i]);

        if (project_target(p, names[i])) {
            snprintf(err, errsz, "%s: duplicate target '%s'", cfg, names[i]);
            goto fail;
        }

        TARGET *t = target_add(p, names[i], norm_dir(pkgdir));

        if (!t || !t->name || !t->dir) {
            snprintf(err, errsz, "out of memory");
            goto fail;
        }

        if (target_fill(p, t, &local, section, err, errsz) != 0) goto fail;
    }

    int pi = 0;
    const char *sub = NULL;

    while ((sub = toml_arr(&local, "package", "deps", pi++))) {
        char   subdir[2048];
        snprintf(subdir, sizeof(subdir), "%s/%s", norm_dir(pkgdir), sub);

        if (!dir_exists(subdir)) {
            snprintf(err, errsz, "%s: missing package directory '%s'",
                     cfg, sub);
            goto fail;
        }

        if (load_package(p, subdir, depth + 1, err, errsz) != 0) goto fail;
    }

    for (int i = 0; i < n; i++) free(names[i]);

    free(names);
    toml_free(&local);
    return 0;

fail:
    for (int i = 0; i < n; i++) free(names[i]);

    free(names);
    toml_free(&local);
    return -1;
}

int project_load(PROJECT *p, const char *root, const char *toolchain,
                 char *err, size_t errsz) {
    memset(p, 0, sizeof(*p));

    p->root = sys_dup(root ? root : ".");

    char cfg[2048];
    snprintf(cfg, sizeof(cfg), "%s/heddle.toml", p->root);

    if (!path_exists(cfg)) {
        snprintf(err, errsz, "cannot read %s", cfg);
        return -1;
    }

    TOML top;
    toml_init(&top);

    if (toml_parse(&top, cfg, err, errsz) != 0) {
        toml_free(&top);
        return -1;
    }

    const char *dir = toml_str(&top, "build", "dir");
    const char *tc  = toolchain ? toolchain
                                : toml_str(&top, "build", "toolchain");

    p->build_dir      = str_join(p->root, dir ? dir : "build");
    p->toolchain_name = sys_dup(tc ? tc : "auto");

    toml_free(&top);

    if (tc_load(&p->tc, p->root, p->toolchain_name, err, errsz) != 0) {
        project_free(p);
        return -1;
    }

    if (!dir_exists(p->build_dir)) sys_mkpath(p->build_dir);

    if (load_package(p, norm_dir(p->root), 0, err, errsz) != 0) {
        project_free(p);
        return -1;
    }

    return 0;
}

void project_free(PROJECT *p) {
    for (int i = 0; i < p->ntargets; i++)
        target_free(&p->targets[i]);

    free(p->targets);
    free(p->root);
    free(p->build_dir);
    free(p->toolchain_name);

    tc_free(&p->tc);

    memset(p, 0, sizeof(*p));
}

const char *target_type_name(TARGET_TYPE t) {
    if (t == TARGET_EXE) return "exe";
    if (t == TARGET_STATICLIB) return "staticlib";

    return "sharedlib";
}

TARGET *project_target(PROJECT *p, const char *name) {
    for (int i = 0; i < p->ntargets; i++)
        if (!strcmp(p->targets[i].name, name)) return &p->targets[i];

    return NULL;
}
