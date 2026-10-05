#include "heddle.h"

#include "build.h"
#include "emit.h"
#include "install.h"
#include "ldconv.h"
#include "pkg.h"
#include "project.h"
#include "toml.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void env_free(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);

    free(v);
}

static int load(const HEDDLE_OPTS *o, PROJECT *p, char *err, size_t errsz) {
    if (sys_chdir(o->root) != 0) {
        snprintf(err, errsz, "cannot enter project directory '%s'", o->root);
        return -1;
    }

    if (project_load(p, ".", o->toolchain, err, errsz) != 0)
        return -1;

    if (project_check(p, err, errsz) != 0) {
        project_free(p);
        return -1;
    }

    if (o->target && !project_target(p, o->target)) {
        snprintf(err, errsz, "unknown target '%s'", o->target);
        project_free(p);
        return -1;
    }

    return 0;
}

int heddle_check(const HEDDLE_OPTS *o) {
    char err[512] = {0};
    PROJECT p;

    if (load(o, &p, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    printf("heddle: %d targets ok, toolchain=%s platform=%s\n",
           p.ntargets, p.tc.name, p.tc.platform);

    for (int i = 0; i < p.ntargets; i++) {
        TARGET *t = &p.targets[i];

        printf("  %-16s %-10s src=%d inc=%d deps=%d\n",
               t->name, target_type_name(t->type),
               t->nsrc, t->ninc, t->ndeps);
    }

    project_free(&p);
    return 0;
}

static int build_target(const HEDDLE_OPTS *o, PROJECT *p, char *err) {
    char cache[4096];
    snprintf(cache, sizeof(cache), ".heddle");

    sys_mkpath(cache);

    char graph[4096 + 64];
    snprintf(graph, sizeof(graph), "%s/%s.graph", cache, o->target);

    if (emit_graph(p, o->target, graph, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    BUILD_OPTS bo;
    bo.graph_file = graph;
    bo.cwd        = NULL;
    bo.logdir     = cache;
    bo.jobs       = o->jobs > 0 ? o->jobs : 1;
    bo.retry      = 0;
    bo.keep_going = 1;
    bo.cache_dir  = o->cache;
    bo.remote     = o->remote;
    bo.no_cache   = o->no_cache;

    int    npk   = 0;
    char **pkenv = pkg_env(&p->pkg, &npk);
    int    nen   = p->tc.nenv + npk;

    char **env = nen ? (char **)malloc(sizeof(char *) * (size_t)nen) : NULL;
    int    n   = 0;

    if (env) {
        for (int i = 0; i < p->tc.nenv; i++) env[n++] = p->tc.env[i];
        for (int i = 0; i < npk; i++)         env[n++] = pkenv[i];
    }

    bo.env  = env;
    bo.nenv = n;

    BUILD_ENGINE *e = build_open(&bo, err, sizeof(err));

    if (!e) {
        fprintf(stderr, "heddle: %s\n", err);
        env_free(pkenv, npk);
        free(env);
        return 1;
    }

    if (o->verbose) {
        char variant[256];

        pkg_variant_key(&p->pkg.target, &p->tc, variant, sizeof(variant));

        fprintf(stderr, "heddle: toolchain=%s platform=%s variant=%s\n",
                p->tc.name, p->tc.platform, variant);
    }

    int rc  = build_run(e);
    int ran = 0;
    int hit = 0;

    build_stats(e, &ran, &hit);

    if (o->verbose)
        fprintf(stderr, "heddle: %d ran, %d cached\n", ran, hit);

    build_close(e);
    env_free(pkenv, npk);
    free(env);
    return rc;
}

int heddle_run(const HEDDLE_OPTS *o) {
    char    err[512] = {0};
    PROJECT p;

    if (load(o, &p, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    int rc = build_target(o, &p, err);

    project_free(&p);
    return rc;
}

int heddle_exec(const HEDDLE_OPTS *o) {
    char  err[512] = {0};
    char  bin[4096];
    PROJECT p;

    if (load(o, &p, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    int rc = build_target(o, &p, err);

    if (rc == 0) {
        char *bin_path = emit_artifact(&p, project_target(&p, o->target));

        if (bin_path) {
            snprintf(bin, sizeof(bin), "%s", bin_path);
            free(bin_path);
        }

        SYS_STAT st;

        int found = sys_stat(bin, &st) == 0;

#ifdef _WIN32
        if (!found) {
            char alt[4096];

            snprintf(alt, sizeof(alt), "%s.exe", bin);
            snprintf(bin, sizeof(bin), "%s", alt);
            found = sys_stat(alt, &st) == 0;
        }
#endif

        if (found) {
            printf("heddle: run %s\n", bin);
            fflush(stdout);

            rc = system(bin) == 0 ? 0 : 1;
        } else {
            fprintf(stderr, "heddle: cannot find '%s'\n", bin);
            rc = 1;
        }
    }

    project_free(&p);
    return rc;
}

int heddle_toolchains(void) {
    printf("detected toolchains (auto uses the first match):\n");

    for (int i = 0; i < tc_auto_count(); i++) {
        const char *name = tc_auto_at(i);

        printf("  %-8s %-4s %s\n", name,
               tc_probe(name) ? "ok" : "-", tc_preset_cc(name));
    }

    return 0;
}

static void apply_registry_env(const HEDDLE_OPTS *o) {
    if (o->registry && o->registry[0]) {
#if defined(_WIN32)
        _putenv_s("HEDDLE_REGISTRY", o->registry);
#else
        setenv("HEDDLE_REGISTRY", o->registry, 1);
#endif
    }
}

static int pkg_load(const HEDDLE_OPTS *o, PKG_MANIFEST *m, char *err,
                    size_t errsz) {
    if (sys_chdir(o->root) != 0) {
        snprintf(err, errsz, "cannot enter project directory '%s'", o->root);
        return -1;
    }

    apply_registry_env(o);

    return pkg_manifest_load(m, ".", err, errsz);
}

int heddle_tool_plan(const HEDDLE_OPTS *o) {
    char         err[512] = {0};
    PKG_MANIFEST m;

    if (pkg_load(o, &m, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    pkg_plan(&m);
    pkg_manifest_free(&m);
    return 0;
}

int heddle_tool_install(const HEDDLE_OPTS *o) {
    char         err[512] = {0};
    PKG_MANIFEST m;

    if (pkg_load(o, &m, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    if (o->verbose) pkg_plan(&m);

    if (pkg_install(&m, o->offline, o->verbose, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        pkg_manifest_free(&m);
        return 1;
    }

    int nt = m.tools.n;
    int nd = m.deps.n;

    printf("heddle: installed %d toolchain package%s and %d dependenc%s\n",
           nt, nt == 1 ? "" : "s", nd, nd == 1 ? "y" : "ies");

    if (nt || nd)
        printf("heddle: store %s\nheddle: lock  %s\n", m.store, m.lock_path);

    pkg_manifest_free(&m);
    return 0;
}

int heddle_env_verify(const HEDDLE_OPTS *o) {
    char         err[512] = {0};
    PKG_MANIFEST m;

    if (pkg_load(o, &m, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    if (pkg_verify(&m, o->verbose, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        pkg_manifest_free(&m);
        return 1;
    }

    printf("heddle: environment matches %s (%d toolchain, %d dependenc%s)\n",
           m.lock_path, m.tools.n, m.deps.n, m.deps.n == 1 ? "y" : "ies");

    pkg_manifest_free(&m);
    return 0;
}

int heddle_ldconv(int argc, char **argv) {
    if (argc < 5) return 2;

    char err[256] = {0};

    if (ldconv_convert(argv[2], argv[3], argv[4], err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: ldconv: %s\n", err);
        return 1;
    }

    return 0;
}

static int resolve_prefix(const HEDDLE_OPTS *o, PROJECT *p, char *out,
                          size_t cap, char *err, size_t errsz) {
    if (o->prefix && o->prefix[0]) {
        snprintf(out, cap, "%s", o->prefix);
        return 0;
    }

    char cfg[4096];
    snprintf(cfg, sizeof(cfg), "%s/heddle.toml", p->root);

    TOML t;
    toml_init(&t);

    if (toml_parse(&t, cfg, err, errsz) == 0) {
        const char *pf = toml_str(&t, "install", "prefix");

        if (pf && pf[0]) {
            snprintf(out, cap, "%s", pf);
            toml_free(&t);
            return 0;
        }

        toml_free(&t);
    }

    snprintf(err, errsz,
             "no install prefix; pass --prefix DIR or set [install] prefix");
    return -1;
}

int heddle_install(const HEDDLE_OPTS *o) {
    char    err[512] = {0};
    PROJECT p;

    if (load(o, &p, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    INSTALL_SET set;
    memset(&set, 0, sizeof(set));

    if (install_load(&set, &p, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        project_free(&p);
        return 1;
    }

    if (!install_has_any(&set)) {
        fprintf(stderr, "heddle: no [target.*.install] rules to install\n");
        install_free(&set);
        project_free(&p);
        return 1;
    }

    char prefix[4096];

    if (resolve_prefix(o, &p, prefix, sizeof(prefix), err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        install_free(&set);
        project_free(&p);
        return 1;
    }

    if (o->target && !project_target(&p, o->target)) {
        fprintf(stderr, "heddle: unknown target '%s'\n", o->target);
        install_free(&set);
        project_free(&p);
        return 1;
    }

    INSTALL_PLAN pl;
    memset(&pl, 0, sizeof(pl));

    if (install_plan(&set, &p, o->target, prefix, o->destdir, &pl,
                     err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        install_plan_free(&pl);
        install_free(&set);
        project_free(&p);
        return 1;
    }

    if (o->dry_run) {
        install_dry_run(&pl);
        install_plan_free(&pl);
        install_free(&set);
        project_free(&p);
        return 0;
    }

    char log[4096];

    install_log_path(log, sizeof(log), p.root, o->destdir, prefix);

    if (install_run(&pl, log, o->verbose, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        install_plan_free(&pl);
        install_free(&set);
        project_free(&p);
        return 1;
    }

    printf("heddle: installed %d file%s to %s%s\n", pl.n, pl.n == 1 ? "" : "s",
           o->destdir ? o->destdir : "", prefix);
    printf("heddle: log %s\n", log);

    install_plan_free(&pl);
    install_free(&set);
    project_free(&p);
    return 0;
}

int heddle_uninstall(const HEDDLE_OPTS *o) {
    char    err[512] = {0};
    PROJECT p;

    if (load(o, &p, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    char prefix[4096];

    if (resolve_prefix(o, &p, prefix, sizeof(prefix), err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        project_free(&p);
        return 1;
    }

    char log[4096];
    install_log_path(log, sizeof(log), p.root, o->destdir, prefix);

    int rc = install_uninstall(log, o->verbose, err, sizeof(err));

    if (rc != 0) fprintf(stderr, "heddle: %s\n", err);

    project_free(&p);
    return rc == 0 ? 0 : 1;
}
