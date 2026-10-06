#include "build.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *file;
    const char *cwd;
    const char *logdir;
    const char *cache_dir;
    const char *remote;
    int         jobs;
    int         retry;
    int         keep_going;
    int         no_cache;
} OPTIONS;

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s -f FILE [-j N] [--retry N] [--cwd DIR] "
            "[--logdir DIR] [--stop] [--cache DIR] [--remote URL] "
            "[--no-cache]\n",
            prog);
}

static int opt_inline(const char *arg, const char *prefix, const char **out) {
    size_t len = strlen(prefix);
    if (strncmp(arg, prefix, len) != 0 || arg[len] == 0) return 0;

    *out = arg + len;
    return 1;
}

static int parse_options(int argc, char **argv, OPTIONS *o) {
    o->file       = NULL;
    o->cwd        = NULL;
    o->logdir     = ".build";
    o->cache_dir  = NULL;
    o->remote     = NULL;
    o->jobs       = 1;
    o->retry      = 0;
    o->keep_going = 1;
    o->no_cache   = 0;
    for (int i = 1; i < argc; i++) {
        const char *value = NULL;
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) {
            printf("loom %s\n", heddle_version_string());
            exit(0);
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            o->file = argv[++i];
        } else if (opt_inline(argv[i], "-f", &value)) {
            o->file = value;
        } else if (!strcmp(argv[i], "-j") && i + 1 < argc) {
            o->jobs = atoi(argv[++i]);
        } else if (opt_inline(argv[i], "-j", &value)) {
            o->jobs = atoi(value);
        } else if (!strcmp(argv[i], "--retry") && i + 1 < argc) {
            o->retry = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--cwd") && i + 1 < argc) {
            o->cwd = argv[++i];
        } else if (!strcmp(argv[i], "--logdir") && i + 1 < argc) {
            o->logdir = argv[++i];
        } else if (!strcmp(argv[i], "--stop")) {
            o->keep_going = 0;
        } else if (!strcmp(argv[i], "--cache") && i + 1 < argc) {
            o->cache_dir = argv[++i];
        } else if (!strcmp(argv[i], "--remote") && i + 1 < argc) {
            o->remote = argv[++i];
        } else if (!strcmp(argv[i], "--no-cache")) {
            o->no_cache = 1;
        } else {
            fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
            return -1;
        }
    }

    return o->file ? 0 : -1;
}

int main(int argc, char **argv) {
    OPTIONS o;
    if (parse_options(argc, argv, &o) != 0) {
        usage(argv[0]);
        return 2;
    }

    if (o.jobs < 1) o.jobs = 1;

    BUILD_OPTS bo;
    bo.graph_file          = o.file;
    bo.cwd                 = o.cwd;
    bo.logdir              = o.logdir;
    bo.jobs                = o.jobs;
    bo.retry               = o.retry;
    bo.keep_going          = o.keep_going;
    bo.cache_dir           = o.cache_dir;
    bo.remote              = o.remote;
    bo.no_cache            = o.no_cache;
    char          err[512] = {0};
    BUILD_ENGINE *e        = build_open(&bo, err, sizeof(err));
    if (!e) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    int rc  = build_run(e);
    int ran = 0;
    int hit = 0;
    build_stats(e, &ran, &hit);
    if (hit) fprintf(stderr, "loom: %d cached, %d ran\n", hit, ran);

    build_close(e);
    return rc;
}
