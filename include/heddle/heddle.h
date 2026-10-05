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
    int         offline;
    const char *registry;
} HEDDLE_OPTS;

int heddle_run(const HEDDLE_OPTS *o);

int heddle_exec(const HEDDLE_OPTS *o);

int heddle_check(const HEDDLE_OPTS *o);

int heddle_toolchains(void);

int heddle_tool_install(const HEDDLE_OPTS *o);
int heddle_tool_plan(const HEDDLE_OPTS *o);

int heddle_env_verify(const HEDDLE_OPTS *o);

#endif
