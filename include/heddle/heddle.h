#ifndef HEDDLE_H
#define HEDDLE_H

typedef struct {
    const char *root;
    const char *target;
    const char *toolchain;
    const char *cache;
    const char *remote;
    int         jobs;
    int         verbose;
    int         no_cache;
} HEDDLE_OPTS;

int heddle_run(const HEDDLE_OPTS *o);

int heddle_exec(const HEDDLE_OPTS *o);

int heddle_check(const HEDDLE_OPTS *o);

int heddle_toolchains(void);

#endif
