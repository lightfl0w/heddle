#ifndef LANG_H
#define LANG_H

#include "toml.h"

typedef struct {
    char **ext;
    int    next;

    char  *cmd;
    char **args;
    int    nargs;

    char *outext;

    int cflags;
    int fmt;
} LANG;

int lang_init_builtin(void);

int lang_load_toml(const TOML *t);

const LANG *lang_for(const char *path);

int lang_add(const char *ext, const char *cmd,
             const char *const *args, int nargs,
             const char *outext, int cflags, int fmt);

int lang_add_ext(LANG *l, const char *ext);

#endif
