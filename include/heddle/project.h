#ifndef PROJECT_H
#define PROJECT_H

#include <stddef.h>

#include "pkg.h"
#include "toolchain.h"
#include "toml.h"

typedef enum {
    TARGET_EXE,
    TARGET_STATICLIB,
    TARGET_SHAREDLIB,
    TARGET_RAW,
    TARGET_CUSTOM
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

    char  *cmd;
    char  *out;

    char  *ldscript;
    char  *entry;
    char  *format;
} TARGET;

typedef struct {
    char *root;
    char *build_dir;
    char *toolchain_name;

    TOOLCHAIN tc;

    PKG_MANIFEST pkg;
    char        *target_prefix;
    char        *target_sysroot;

    char        *tc_based;
    char        *tc_cc;
    char        *tc_family;

    TARGET *targets;
    int     ntargets;
    int     cap;
} PROJECT;

void project_set_target(const char *name);

int project_load(PROJECT *p, const char *root, const char *toolchain,
                  char *err, size_t errsz);

void project_free(PROJECT *p);

TARGET *project_target(PROJECT *p, const char *name);

const char *target_type_name(TARGET_TYPE t);

char *project_path(const char *root, const char *rel);


int project_check(const PROJECT *p, char *err, size_t errsz);

#endif
