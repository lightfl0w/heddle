#ifndef PROJECT_H
#define PROJECT_H

#include <stddef.h>

#include "toolchain.h"
#include "toml.h"

typedef enum {
    TARGET_EXE,
    TARGET_STATICLIB,
    TARGET_SHAREDLIB
} TARGET_TYPE;

typedef struct {
    char *name;
    char *dir;

    TARGET_TYPE type;

    char **src;
    int    nsrc;

    char **inc;
    int    ninc;

    char **deps;
    int    ndeps;

    char **cflags;
    int    ncflags;

    char **ldflags;
    int    nldflags;
} TARGET;

typedef struct {
    char *root;
    char *build_dir;
    char *toolchain_name;

    TOOLCHAIN tc;

    TARGET *targets;
    int     ntargets;
    int     cap;
} PROJECT;

int project_load(PROJECT *p, const char *root, const char *toolchain,
                  char *err, size_t errsz);

void project_free(PROJECT *p);

TARGET *project_target(PROJECT *p, const char *name);

const char *target_type_name(TARGET_TYPE t);


int project_check(const PROJECT *p, char *err, size_t errsz);

#endif
