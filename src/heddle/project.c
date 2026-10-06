#include "glob.h"
#include "lang.h"
#include "project.h"
#include "recipe.h"
#include "star.h"
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

    *v       = next;
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
        int     newcap  = p->cap ? p->cap * 2 : 16;
        TARGET *targets = (TARGET *)realloc(p->targets, sizeof(TARGET) * (size_t)newcap);
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
    free(t->entry);
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

    if (!strcmp(s, "sharedlib") || !strcmp(s, "dylib") || !strcmp(s, "so")) {
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

static int load_strings(const TOML *cfg, const char *section, const char *key, const char *dir,
                        char ***out, int *n) {
    for (int i = 0;; i++) {
        const char *v = toml_arr(cfg, section, key, i);
        if (!v) return 0;

        char *full = dir ? prefixed(dir, v) : sys_dup(v);
        if (!full) return -1;

        vec_add(out, n, full);
        free(full);
    }
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int load_sources(const TOML *cfg, const char *section, const char *dir, char ***out, int *n,
                        char *err, size_t errsz) {
    for (int i = 0;; i++) {
        const char *v = toml_arr(cfg, section, "src", i);
        if (!v) break;

        if (!glob_has_wild(v)) {
            char *full = dir ? prefixed(dir, v) : sys_dup(v);
            if (full) {
                vec_add(out, n, full);
                free(full);
            }

            continue;
        }

        GLOB_LIST l;
        glob_init(&l);
        int hit = glob_dir(dir && dir[0] ? dir : ".", v, &l);
        if (hit == 0) {
            snprintf(err, errsz, "section [%s]: pattern '%s' matches nothing", section, v);
            glob_free(&l);
            return -1;
        }

        for (int k = 0; k < l.n; k++) {
            char *full = dir ? prefixed(dir, l.items[k]) : sys_dup(l.items[k]);
            if (full) {
                vec_add(out, n, full);
                free(full);
            }
        }

        glob_free(&l);
    }

    if (*n > 1) {
        qsort(*out, (size_t)*n, sizeof(char *), cmp_str);
        int w = 1;
        for (int i = 1; i < *n; i++) {
            if (strcmp((*out)[i], (*out)[w - 1])) {
                (*out)[w++] = (*out)[i];
            } else {
                free((*out)[i]);
            }
        }

        *n = w;
    }

    return 0;
}

static int target_fill(TARGET *t, const TOML *cfg, const char *section, char *err, size_t errsz) {
    const char *type = toml_str(cfg, section, "type");
    if (!type) {
        snprintf(err, errsz, "section [%s]: missing key 'type'", section);
        return -1;
    }

    if (parse_type(type, &t->type) != 0) {
        snprintf(err, errsz, "section [%s]: unknown type '%s'", section, type);
        return -1;
    }

    if (load_sources(cfg, section, t->dir, &t->src, &t->nsrc, err, errsz) != 0) return -1;

    if (load_strings(cfg, section, "inc", t->dir, &t->inc, &t->ninc) != 0) return -1;

    if (load_strings(cfg, section, "deps", NULL, &t->deps, &t->ndeps) != 0) return -1;

    if (load_strings(cfg, section, "cflags", NULL, &t->cflags, &t->ncflags) != 0) return -1;

    if (load_strings(cfg, section, "ldflags", NULL, &t->ldflags, &t->nldflags) != 0) return -1;

    if (t->type != TARGET_CUSTOM && t->nsrc == 0) {
        snprintf(err, errsz, "section [%s]: key 'src' is empty", section);
        return -1;
    }

    const char *lds   = toml_str(cfg, section, "linker_script");
    const char *entry = toml_str(cfg, section, "entry");
    const char *fmt   = toml_str(cfg, section, "format");
    const char *out   = toml_str(cfg, section, "out");
    t->ldscript       = lds ? project_path(t->dir, lds) : NULL;
    t->entry          = entry ? sys_dup(entry) : NULL;
    t->format         = fmt ? sys_dup(fmt) : NULL;
    t->out            = out ? sys_dup(out) : NULL;
    if (t->type == TARGET_CUSTOM) {
        const char *cmd = toml_str(cfg, section, "cmd");
        if (!cmd || !t->out) {
            snprintf(err, errsz, "section [%s]: custom needs 'cmd' and 'out'", section);
            return -1;
        }

        t->cmd = project_path(t->dir, cmd);
    }

    if (t->type == TARGET_RAW && !t->format) {
        snprintf(err, errsz, "section [%s]: raw needs 'format' (e.g. bin, elf)", section);
        return -1;
    }

    return 0;
}

static int deps_has(const PROJECT *p, const char *name) {
    for (int i = 0; i < p->ntargets; i++)
        if (!strcmp(p->targets[i].name, name)) return 1;

    return 0;
}

static int load_recipe_target(PROJECT *p, RECIPE *r, const char *src_dir, char *err, size_t errsz) {
    if (deps_has(p, r->pkg.name)) return 0;

    TARGET *t = target_add(p, r->pkg.name, src_dir);
    if (!t || !t->name || !t->dir) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    t->type = TARGET_STATICLIB;
    if (r->build.type && parse_type(r->build.type, &t->type) != 0) {
        snprintf(err, errsz, "%s: unknown build type '%s'", r->pkg.name, r->build.type);
        return -1;
    }

    for (int i = 0; i < r->build.nfile; i++) vec_add(&t->src, &t->nsrc, r->build.files[i]);

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

    for (int i = 0; i < r->build.ncflags; i++) vec_add(&t->cflags, &t->ncflags, r->build.cflags[i]);

    return 0;
}

static const char *norm_dir(const char *d) {
    while (d[0] == '.' && d[1] == '/') d += 2;

    return d;
}

static int load_package(PROJECT *p, const char *pkgdir, int depth, char *err, size_t errsz) {
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
    int    n     = toml_sections(&local, "target.", &names);
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

    int         pi  = 0;
    const char *sub = NULL;
    while ((sub = toml_arr(&local, "package", "deps", pi++))) {
        char *subdir = project_path(norm_dir(pkgdir), sub);
        if (!subdir || !dir_exists(subdir)) {
            snprintf(err, errsz, "%s: missing package directory '%s'", cfg, sub);
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

static char *dup_opt(const char *s) {
    return s ? sys_dup(s) : NULL;
}

static const char *g_want_target;

void project_set_target(const char *name) {
    g_want_target = name;
}

static int add_target_from_cfg(PROJECT *p, STAR_TARGET *st, char *err, size_t errsz) {
    if (project_target(p, st->name)) {
        snprintf(err, errsz, "duplicate target '%s'", st->name);
        return -1;
    }

    TARGET *t = target_add(p, st->name, "");
    if (!t) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    if (st->type && parse_type(st->type, &t->type) != 0) {
        snprintf(err, errsz, "target '%s': unknown type '%s'", st->name, st->type);
        return -1;
    }

    for (int i = 0; i < st->nsrc; i++) vec_add(&t->src, &t->nsrc, st->src[i]);
    for (int i = 0; i < st->ninc; i++) vec_add(&t->inc, &t->ninc, st->inc[i]);
    for (int i = 0; i < st->ndeps; i++) vec_add(&t->deps, &t->ndeps, st->deps[i]);
    for (int i = 0; i < st->ncflags; i++) vec_add(&t->cflags, &t->ncflags, st->cflags[i]);
    for (int i = 0; i < st->nldflags; i++) vec_add(&t->ldflags, &t->nldflags, st->ldflags[i]);

    if (st->ldscript) t->ldscript = sys_dup(st->ldscript);
    if (st->entry) t->entry = sys_dup(st->entry);
    if (st->out) t->out = sys_dup(st->out);

    if (t->type != TARGET_CUSTOM && t->nsrc == 0) {
        snprintf(err, errsz, "target '%s': no sources", st->name);
        return -1;
    }

    return 0;
}

static int project_load_star(PROJECT *p, const char *root, const char *toolchain, char *err,
                             size_t errsz) {
    char cfgpath[2048];
    snprintf(cfgpath, sizeof(cfgpath), "%s/heddle.star", root);
    STAR_CFG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.root = sys_dup(root);
    if (star_run(cfgpath, &cfg, err, errsz) != 0) {
        star_cfg_free(&cfg);
        return -1;
    }

    const STAR_PLATFORM *plat = NULL;
    if (g_want_target) {
        for (int i = 0; i < cfg.ntg; i++) {
            if (strcmp(cfg.tg[i].name, g_want_target)) continue;
            if (!cfg.tg[i].platform) continue;

            for (int k = 0; k < cfg.npl; k++)
                if (!strcmp(cfg.pl[k].name, cfg.tg[i].platform)) plat = &cfg.pl[k];
        }
    }

    if (!plat)
        for (int i = 0; i < cfg.ntg; i++)
            if (cfg.tg[i].platform) {
                for (int k = 0; k < cfg.npl; k++)
                    if (!strcmp(cfg.pl[k].name, cfg.tg[i].platform)) plat = &cfg.pl[k];
            }

    if (!plat && cfg.npl) plat = &cfg.pl[0];

    if (cfg.npl) {
        for (int i = 0; i < cfg.ntg; i++) {
            if (!cfg.tg[i].platform) continue;

            for (int k = 0; k < cfg.npl; k++) {
                const STAR_PLATFORM *q = &cfg.pl[k];
                if (strcmp(q->name, cfg.tg[i].platform)) continue;

                if (plat && q->toolchain && plat->toolchain &&
                    strcmp(q->toolchain, plat->toolchain)) {
                    snprintf(err, errsz,
                             "targets span toolchains '%s' and '%s'; "
                             "build one platform at a time",
                             plat->toolchain, q->toolchain);
                    star_cfg_free(&cfg);
                    return -1;
                }
            }
        }
    }

    p->build_dir       = project_path(root, cfg.build_dir ? cfg.build_dir : "out");
    const char *tcname = toolchain
                             ? toolchain
                             : (plat && plat->toolchain ? plat->toolchain
                                                        : (cfg.toolchain ? cfg.toolchain : "auto"));
    p->toolchain_name  = sys_dup(tcname);
    for (int i = 0; i < cfg.ntc; i++) {
        if (strcmp(cfg.tc_name[i], tcname)) continue;

        p->tc_based = dup_opt(cfg.tc_based[i]);
        p->tc_cc    = dup_opt(cfg.tc_cc[i]);
    }

    p->pkg.root      = sys_dup(root);
    p->pkg.manifest  = sys_dup(cfgpath);
    p->pkg.lock_path = project_path(root, "heddle.lock");
    p->pkg.store = cfg.store ? project_path(root, cfg.store) : project_path(root, ".heddle/store");
    p->pkg.target.arch    = dup_opt(plat && plat->arch ? plat->arch : cfg.arch);
    p->pkg.target.abi     = dup_opt(plat && plat->abi ? plat->abi : cfg.abi);
    p->pkg.target.float_k = dup_opt(plat && plat->flt ? plat->flt : cfg.flt);
    p->pkg.target.sysroot = dup_opt(plat && plat->sysroot ? plat->sysroot : cfg.sysroot);
    for (int i = 0; i < cfg.ntc; i++)
        pkg_manifest_add(&p->pkg, PKG_KIND_TOOLCHAIN, cfg.tc_name[i], NULL, cfg.tc_cc[i]);
    for (int i = 0; i < cfg.ndep; i++)
        pkg_manifest_add(&p->pkg, PKG_KIND_LIBRARY, cfg.dep_name[i], cfg.dep_ver[i],
                         cfg.dep_src[i]);
    for (int i = 0; i < cfg.ntg; i++)
        if (add_target_from_cfg(p, &cfg.tg[i], err, errsz) != 0) {
            star_cfg_free(&cfg);
            return -1;
        }

    star_cfg_free(&cfg);
    return 0;
}

static int project_load_star_finish(PROJECT *p, const char *root, const char *toolchain, char *err,
                                    size_t errsz);

int project_load(PROJECT *p, const char *root, const char *toolchain, char *err, size_t errsz) {
    memset(p, 0, sizeof(*p));
    p->root = sys_dup(root ? root : ".");
    char star[2048];
    char cfg[2048];
    snprintf(star, sizeof(star), "%s/heddle.star", p->root);
    snprintf(cfg, sizeof(cfg), "%s/heddle.toml", p->root);
    if (path_exists(star)) return project_load_star_finish(p, p->root, toolchain, err, errsz);

    if (!path_exists(cfg)) {
        snprintf(err, errsz, "cannot read %s or %s", cfg, star);
        return -1;
    }

    TOML top;
    toml_init(&top);
    if (toml_parse(&top, cfg, err, errsz) != 0) {
        toml_free(&top);
        return -1;
    }

    const char *dir   = toml_str(&top, "build", "dir");
    const char *tc    = toolchain ? toolchain : toml_str(&top, "build", "toolchain");
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
    snprintf(target_flags, sizeof(target_flags), "%s %s", p->pkg.target.cpu, p->pkg.target.fpu);
    if (tc_load_ex(&p->tc, p->root, p->toolchain_name, p->target_prefix, p->target_sysroot,
                   target_flags, p->pkg.target.arch, err, errsz) != 0) {
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
    for (int i = 0; i < p->ntargets; i++) target_free(&p->targets[i]);

    free(p->targets);
    free(p->root);
    free(p->build_dir);
    free(p->toolchain_name);
    free(p->target_prefix);
    free(p->target_sysroot);
    free(p->tc_based);
    free(p->tc_cc);
    free(p->tc_family);
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

static int project_load_star_finish(PROJECT *p, const char *root, const char *toolchain, char *err,
                                    size_t errsz) {
    lang_init_builtin();
    if (project_load_star(p, root, toolchain, err, errsz) != 0) {
        project_free(p);
        return -1;
    }

    if (pkg_manifest_finalize(&p->pkg, err, errsz) != 0) {
        project_free(p);
        return -1;
    }

    p->target_prefix  = sys_dup(p->pkg.toolchain_prefix);
    p->target_sysroot = sys_dup(p->pkg.sysroot);
    pkg_prepend_path(&p->pkg);
    char flags[1024];
    snprintf(flags, sizeof(flags), "%s %s", p->pkg.target.cpu, p->pkg.target.fpu);
    int trc;
    if (p->tc_cc || p->tc_based)
        trc = tc_load_star(&p->tc, p->toolchain_name, p->tc_based, p->tc_cc, p->tc_family,
                           p->target_prefix, p->target_sysroot, flags, p->pkg.target.arch, err,
                           errsz);
    else
        trc = tc_load_ex(&p->tc, root, p->toolchain_name, p->target_prefix, p->target_sysroot,
                         flags, p->pkg.target.arch, err, errsz);
    if (trc != 0) {
        project_free(p);
        return -1;
    }

    if (!dir_exists(p->build_dir)) sys_mkpath(p->build_dir);

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
