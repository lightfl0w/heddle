#include "graph.h"
#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <direct.h>
#define MKDIR(p) _mkdir(p)

#else

#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(p) mkdir((p), 0755)

#endif

typedef struct {
    const char *file;
    const char *cwd;
    const char *logdir;
    int         jobs;
    int         retry;
    int         keep_going;
} OPTIONS;

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = 0;

    fclose(f);
    return buf;
}

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

    char *text = read_file(o.file);
    if (!text) {
        fprintf(stderr, "error: cannot read %s\n", o.file);
        return 1;
    }

    GRAPH g;
    char  err[512] = {0};

    if (graph_parse(text, &g, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        free(text);
        return 1;
    }
    free(text);

    MKDIR(o.logdir);

    SCHED_OPTS so;
    so.g          = &g;
    so.cwd        = o.cwd;
    so.logdir     = o.logdir;
    so.jobs       = o.jobs;
    so.retry      = o.retry;
    so.keep_going = o.keep_going;

    int rc = sched_run(&so);

    graph_free(&g);
    return rc;
}
