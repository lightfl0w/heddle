#ifndef HEDDLE_MIGRATE_H
#define HEDDLE_MIGRATE_H

#include <stddef.h>

typedef struct {
    char *name;
    char *type;
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
    char  *ldscript;
} MIG_TARGET;

typedef struct {
    MIG_TARGET *items;
    int         n;
    int         cap;

    char **warn;
    int    nwarn;
} MIG_SET;

int  migrate_load_cmake(MIG_SET *s, const char *path, char *err, size_t errsz);
int  migrate_load_xmake(MIG_SET *s, const char *path, char *err, size_t errsz);

void migrate_free(MIG_SET *s);

int  migrate_write(const MIG_SET *s, const char *out, char *err, size_t errsz);

void migrate_print(const MIG_SET *s);

int  heddle_migrate(int argc, char **argv);

#endif
