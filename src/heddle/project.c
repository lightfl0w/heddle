#include "lang.h"
#include "project.h"
#include "recipe.h"
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

char *project_path(const char *a, const char *b) {
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

    free(t->cmd);
    free(t->out);
    free(t->ldscript);
    free(t->format);

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

    if (!strcmp(s, "raw")) {
        *out = TARGET_RAW;
        return 0;
    }

    if (!strcmp(s, "custom")) {
        *out = TARGET_CUSTOM;
        return 0;
    }

    return -1;
}

static char *prefixed(const char *dir, const char *rel) {
    if (rel[0] == '/') return sys_dup(rel);
    if (!dir || !dir[0]) return sys_dup(rel);

    return project_path(dir, rel);
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

static int target_fill(TARGET *t, const TOML *cfg,
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

    if (t->type != TARGET_CUSTOM && t->nsrc == 0) {
        snprintf(err, errsz, "section [%s]: key 'src' is empty", section);
        return -1;
    }

    const char *lds = toml_str(cfg, section, "linker_script");
    const char *fmt = toml_str(cfg, section, "format");
    const char *out = toml_str(cfg, section, "out");

    t->ldscript = lds ? project_path(t->dir, lds) : NULL;
    t->format   = fmt ? sys_dup(fmt) : NULL;
    t->out      = out ? sys_dup(out) : NULL;

    if (t->type == TARGET_CUSTOM) {
        const char *cmd = toml_str(cfg, section, "cmd");

        if (!cmd || !t->out) {
            snprintf(err, errsz,
                     "section [%s]: custom needs 'cmd' and 'out'", section);
            return -1;
        }

        t->cmd = project_path(t->dir, cmd);
    }

    if (t->type == TARGET_RAW && !t->format) {
        snprintf(err, errsz,
                 "section [%s]: raw needs 'format' (e.g. bin, elf)", section);
        return -1;
    }

    return 0;
}

static int deps_has(const PROJECT *p, const char *name) {
    for (int i = 0; i < p->ntargets; i++)
        if (!strcmp(p->targets[i].name, name)) return 1;

    return 0;
}

static int load_recipe_target(PROJECT *p, RECIPE *r, const char *src_dir,
                              char *err, size_t errsz) {
    if (deps_has(p, r->pkg.name)) return 0;

    TARGET *t = target_add(p, r->pkg.name, src_dir);

    if (!t || !t->name || !t->dir) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    t->type = TARGET_STATICLIB;

    if (r->build.type && parse_type(r->build.type, &t->type) != 0) {
        snprintf(err, errsz, "%s: unknown build type '%s'",
                 r->pkg.name, r->build.type);
        return -1;
    }

    for (int i = 0; i < r->build.nfile; i++)
        vec_add(&t->src, &t->nsrc, r->build.files[i]);

    for (int i = 0; i < r->build.ninc; i++) {
        char *full = prefixed(src_dir, r->build.include_dirs[i]);

        if (full) {
            vec_add(&t->inc, &t->ninc, full);
            free(full);
        }
    }

    for (int i = 0; i < r->build.ndef; i++) {
        char d[1024];

        snprintf(d, sizeof(d), "-D%s", r->build.defines[i]);
        vec_add(&t->cflags, &t->ncflags, d);
    }

    for (int i = 0; i < r->build.ncflags; i++)
        vec_add(&t->cflags, &t->ncflags, r->build.cflags[i]);

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

    char *cfg = project_path(pkgdir, "heddle.toml");

    if (!cfg || !path_exists(cfg)) {
        snprintf(err, errsz, "cannot read %s/heddle.toml", pkgdir);
        free(cfg);
        return -1;
    }

    TOML local;
    toml_init(&local);

    if (toml_parse(&local, cfg, err, errsz) != 0) {
        toml_free(&local);
        free(cfg);
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

        if (target_fill(t, &local, section, err, errsz) != 0) goto fail;
    }

    int pi = 0;
    const char *sub = NULL;

    while ((sub = toml_arr(&local, "package", "deps", pi++))) {
        char *subdir = project_path(norm_dir(pkgdir), sub);

        if (!subdir || !dir_exists(subdir)) {
            snprintf(err, errsz, "%s: missing package directory '%s'",
                     cfg, sub);
            free(subdir);
            goto fail;
        }

        int rc = load_package(p, subdir, depth + 1, err, errsz);

        free(subdir);

        if (rc != 0) goto fail;
    }

    for (int i = 0; i < n; i++) free(names[i]);

    free(names);
    toml_free(&local);
    free(cfg);
    return 0;

fail:
    for (int i = 0; i < n; i++) free(names[i]);

    free(names);
    toml_free(&local);
    free(cfg);
    return -1;
}

static int load_store_recipes(PROJECT *p, char *err, size_t errsz) {
    for (int i = 0; i < p->pkg.deps.n; i++) {
        PKG_SPEC *s = &p->pkg.deps.items[i];

        if (!s->store_path || !recipe_match(s->store_path)) continue;

        RECIPE r;

        if (recipe_load(&r, s->store_path, err, errsz) != 0) return -1;

        int rc = load_recipe_target(p, &r, s->store_path, err, errsz);

        recipe_free(&r);

        if (rc != 0) return -1;
    }

    return 0;
}

static int auto_depends(PROJECT *p, char *err, size_t errsz) {
    for (int i = 0; i < p->pkg.deps.n; i++) {
        const char *name = p->pkg.deps.items[i].name;

        if (!project_target(p, name)) continue;

        for (int k = 0; k < p->ntargets; k++) {
            TARGET *t = &p->targets[k];

            if (t->type != TARGET_EXE && t->type != TARGET_SHAREDLIB) continue;
            if (!strcmp(t->name, name)) continue;

            int have = 0;

            for (int d = 0; d < t->ndeps; d++)
                if (!strcmp(t->deps[d], name)) have = 1;

            if (have) continue;

            if (vec_add(&t->deps, &t->ndeps, name) != 0) {
                snprintf(err, errsz, "out of memory");
                return -1;
            }
        }
    }

    return 0;
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

    p->build_dir      = project_path(p->root, dir ? dir : "build");
    p->toolchain_name = sys_dup(tc ? tc : "auto");

    lang_init_builtin();
    lang_load_toml(&top);

    toml_free(&top);

    if (pkg_manifest_load(&p->pkg, p->root, err, errsz) != 0) {
        project_free(p);
        return -1;
    }

    p->target_prefix  = sys_dup(p->pkg.toolchain_prefix);
    p->target_sysroot = sys_dup(p->pkg.sysroot);

    pkg_prepend_path(&p->pkg);

    char target_flags[1024];
    snprintf(target_flags, sizeof(target_flags), "%s %s",
             p->pkg.target.cpu, p->pkg.target.fpu);

    if (tc_load_ex(&p->tc, p->root, p->toolchain_name,
                   p->target_prefix, p->target_sysroot,
                   target_flags, err, errsz) != 0) {
        project_free(p);
        return -1;
    }

    if (p->pkg.tools.n > 0 && !tc_tool_ok(&p->tc, p->tc.cc)) {
        snprintf(err, errsz,
                 "managed toolchain '%s' is not installed "
                 "(compiler '%s' not found); run 'heddle tool install'",
                 p->pkg.tools.items[0].name, p->tc.cc);
        project_free(p);
        return -1;
    }

    if (!dir_exists(p->build_dir)) sys_mkpath(p->build_dir);

    if (load_package(p, norm_dir(p->root), 0, err, errsz) != 0) {
        project_free(p);
        return -1;
    }

    if (load_store_recipes(p, err, errsz) != 0) {
        project_free(p);
        return -1;
    }

    if (auto_depends(p, err, errsz) != 0) {
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

    free(p->target_prefix);
    free(p->target_sysroot);

    pkg_manifest_free(&p->pkg);

    tc_free(&p->tc);

    memset(p, 0, sizeof(*p));
}

const char *target_type_name(TARGET_TYPE t) {
    if (t == TARGET_EXE) return "exe";
    if (t == TARGET_STATICLIB) return "staticlib";
    if (t == TARGET_SHAREDLIB) return "sharedlib";
    if (t == TARGET_RAW) return "raw";

    return "custom";
}

TARGET *project_target(PROJECT *p, const char *name) {
    for (int i = 0; i < p->ntargets; i++)
        if (!strcmp(p->targets[i].name, name)) return &p->targets[i];

    return NULL;
}
