#ifndef SCHED_H
#define SCHED_H

#include "graph.h"

typedef struct {
    const GRAPH *g;
    const char  *cwd;
    const char  *logdir;
    int          jobs;
    int          retry;
    int          keep_going;
    const char  *active;

    char *const *env;
    int          nenv;
} SCHED_OPTS;

int sched_run(const SCHED_OPTS *opts);

#endif
