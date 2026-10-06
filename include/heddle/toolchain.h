#ifndef TOOLCHAIN_H
#define TOOLCHAIN_H

#include <stddef.h>
#include <stdarg.h>

typedef struct TOOLCHAIN {
    char *name;
    char *family;

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

    char **env;
    int    nenv;
} TOOLCHAIN;

int  tc_load(TOOLCHAIN *tc, const char *dir, const char *name,
             char *err, size_t errsz);

int  tc_load_ex(TOOLCHAIN *tc, const char *dir, const char *name,
                const char *prefix, const char *sysroot,
                const char *extra_cflags, const char *arch,
                char *err, size_t errsz);

int  tc_load_star(TOOLCHAIN *tc, const char *name, const char *based,
                  const char *cc, const char *family,
                  const char *prefix, const char *sysroot,
                  const char *extra_cflags, const char *arch,
                  char *err, size_t errsz);

void tc_free(TOOLCHAIN *tc);

void tc_add_env(TOOLCHAIN *tc, const char *fmt, ...);

void tc_add_env_raw(TOOLCHAIN *tc, const char *s);

int tc_auto_count(void);

const char *tc_auto_at(int i);

int  tc_probe(const char *preset);

const char *tc_tool(const TOOLCHAIN *tc, const char *name);

int  tc_tool_ok(const TOOLCHAIN *tc, const char *tool);

const char *tc_preset_cc(const char *preset);

int tc_probe_triple(const char *cc, char *out, size_t cap);

int tc_triple_needs_msvc(const char *triple);

#endif
