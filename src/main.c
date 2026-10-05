#include "build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *file;
    const char *cwd;
    const char *logdir;
    int         jobs;
    int         retry;
    int         keep_going;
} OPTIONS;

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s -f FILE [-j N] [--retry N] [--cwd DIR] "
            "[--logdir DIR] [--stop]\n", prog);
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
    o->jobs       = 1;
    o->retry      = 0;
    o->keep_going = 1;

    for (int i = 1; i < argc; i++) {
        const char *value = NULL;

        if (!strcmp(argv[i], "-f") && i + 1 < argc) {
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
    bo.graph_file = o.file;
    bo.cwd        = o.cwd;
    bo.logdir     = o.logdir;
    bo.jobs       = o.jobs;
    bo.retry      = o.retry;
    bo.keep_going = o.keep_going;

    char err[512] = {0};

    BUILD_ENGINE *e = build_open(&bo, err, sizeof(err));
    if (!e) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    int rc = build_run(e);

    build_close(e);
    return rc;
}
