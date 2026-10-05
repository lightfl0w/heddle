#ifndef HEDDLE_LINK_H
#define HEDDLE_LINK_H

#include <stddef.h>

#include "toolchain.h"

typedef struct {
    const char *out;
    char      **objs;
    int         nobj;
    char      **libs;
    int         nlib;
    char      **ldflags;
    int         nldf;
    const char *ldscript;
    const char *entry;
    int         shared;
} LINK_REQ;

void link_cmd(const TOOLCHAIN *tc, const LINK_REQ *r, char *buf, size_t cap);

#endif
