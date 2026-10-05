#ifndef TOOLCHAIN_H
#define TOOLCHAIN_H

#include <stddef.h>

typedef struct TOOLCHAIN {
    char *name;

    char *cc;
    char *cxx;
    char *ar;
    char *ld;

    char **cflags;
    int    ncflags;

    char **ldflags;
    int    nldflags;

    char *objext;
    char *binext;
    char *libext;
    char *dllpre;
    char *dllext;
    char *soflag;

    char *as;

    char *platform;
} TOOLCHAIN;

int  tc_load(TOOLCHAIN *tc, const char *dir, const char *name,
             char *err, size_t errsz);

void tc_free(TOOLCHAIN *tc);

int tc_auto_count(void);

const char *tc_auto_at(int i);

int  tc_probe(const char *preset);

const char *tc_tool(const TOOLCHAIN *tc, const char *name);

const char *tc_preset_cc(const char *preset);

#endif
