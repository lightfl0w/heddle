#ifndef BUILD_H
#define BUILD_H

#include <stddef.h>

typedef struct {
    const char *graph_file;
    const char *cwd;
    const char *logdir;
    int         jobs;
    int         retry;
    int         keep_going;
} BUILD_OPTS;

typedef struct BUILD_ENGINE BUILD_ENGINE;

BUILD_ENGINE *build_open(const BUILD_OPTS *o, char *err, size_t errsz);

void build_close(BUILD_ENGINE *e);

int build_run(BUILD_ENGINE *e);

#endif
