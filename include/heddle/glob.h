#ifndef HEDDLE_GLOB_H
#define HEDDLE_GLOB_H

#include <stddef.h>

typedef struct {
    char **items;
    int    n;
    int    cap;
} GLOB_LIST;

void glob_init(GLOB_LIST *l);
void glob_free(GLOB_LIST *l);

int glob_match(const char *pat, const char *name);

int glob_dir(const char *dir, const char *pat, GLOB_LIST *out);

int glob_has_wild(const char *s);

#endif
