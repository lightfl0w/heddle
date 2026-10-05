#include "heddle.h"

#include "build.h"
#include "emit.h"
#include "project.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
    bo.env        = p->tc.env;
    bo.nenv       = p->tc.nenv;

    BUILD_ENGINE *e = build_open(&bo, err, sizeof(err));

    if (!e) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    if (o->verbose)
        fprintf(stderr, "heddle: toolchain=%s platform=%s\n",
                p->tc.name, p->tc.platform);

    int rc  = build_run(e);
    int ran = 0;
    int hit = 0;

    build_stats(e, &ran, &hit);

    if (o->verbose)
        fprintf(stderr, "heddle: %d ran, %d cached\n", ran, hit);

    build_close(e);
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
