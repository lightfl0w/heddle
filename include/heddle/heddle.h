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
    const char *prefix;
    const char *destdir;
    int         dry_run;
    const char *compile_db;
    int         interval_ms;
} HEDDLE_OPTS;

int heddle_run(const HEDDLE_OPTS *o);

int heddle_watch(const HEDDLE_OPTS *o);

int heddle_exec(const HEDDLE_OPTS *o);

int heddle_check(const HEDDLE_OPTS *o);

int heddle_toolchains(void);

int heddle_tool_install(const HEDDLE_OPTS *o);
int heddle_tool_plan(const HEDDLE_OPTS *o);

int heddle_env_verify(const HEDDLE_OPTS *o);

int heddle_ldconv(int argc, char **argv);

int heddle_install(const HEDDLE_OPTS *o);
int heddle_uninstall(const HEDDLE_OPTS *o);

int heddle_compdb(const HEDDLE_OPTS *o);

#endif
