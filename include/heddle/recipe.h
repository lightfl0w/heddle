#ifndef HEDDLE_RECIPE_H
#define HEDDLE_RECIPE_H

#include <stddef.h>

typedef struct {
    char *name;
    char *version;
} RECIPE_PKG;

typedef struct {
    char *url;
    char *tag;
    char *path;
} RECIPE_SOURCE;

typedef struct {
    char **patterns;
    int    npat;
    char **files;
    int    nfile;
    char **include_dirs;
    int    ninc;
    char **defines;
    int    ndef;
    char **cflags;
    int    ncflags;
    char  *type;
} RECIPE_BUILD;

typedef struct {
    RECIPE_PKG    pkg;
    RECIPE_SOURCE source;
    RECIPE_BUILD  build;
    char         *dir;
} RECIPE;

int  recipe_match(const char *dir);
int  recipe_load(RECIPE *r, const char *dir, char *err, size_t errsz);
void recipe_free(RECIPE *r);

#endif
