#ifndef HEDDLE_TEST_H
#define HEDDLE_TEST_H

#include <stddef.h>

#include "heddle.h"
#include "project.h"

typedef struct {
    char *name;
    char **args;
    int    nargs;
    int    expect;
    char  *stdout_match;
} TEST_CASE;

typedef struct {
    TEST_CASE *items;
    int        n;
    int        cap;
} TEST_SET;

int test_load(TEST_SET *s, const PROJECT *p, char *err, size_t errsz);
void test_free(TEST_SET *s);
int heddle_test(const HEDDLE_OPTS *o);

#endif
