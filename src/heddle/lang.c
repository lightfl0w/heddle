#include "lang.h"
#include "sys.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LANG_MAX 32
#define ARGS_MAX 16

static LANG g_langs[LANG_MAX];
static int  g_n;

typedef struct {
    const char  *ext;
    const char  *cmd;
    const char *const *args;
    int          nargs;
    const char  *outext;
    int          cflags;
    int          fmt;
} BUILTIN;

static const char *const cc_args[] = { "-c" };

static const BUILTIN g_builtin[] = {
    { "c",   "cc",   cc_args, 1, ".o", 1, 0 },
    { "cc",  "c++",  cc_args, 1, ".o", 1, 0 },
    { "cpp", "c++",  cc_args, 1, ".o", 1, 0 },
    { "cxx", "c++",  cc_args, 1, ".o", 1, 0 },
    { "c++", "c++",  cc_args, 1, ".o", 1, 0 },
    { "S",   "cc",   cc_args, 1, ".o", 1, 0 },
    { "s",   "cc",   cc_args, 1, ".o", 1, 0 },
    { "asm", "nasm", NULL,    0, ".o", 0, 1 },
};

static int ext_match(const char *path, const char *ext) {
    size_t n = strlen(path);
    size_t m = strlen(ext);

    if (m == 0 || n < m + 1) return 0;
    if (path[n - m - 1] != '.') return 0;

    return !strcmp(path + n - m, ext);
}

const LANG *lang_for(const char *path) {
    for (int i = 0; i < g_n; i++)
        for (int k = 0; k < g_langs[i].next; k++)
            if (ext_match(path, g_langs[i].ext[k])) return &g_langs[i];

    return NULL;
}

static int push_str(char ***v, int *n, const char *s) {
    char **next = (char **)realloc(*v, sizeof(char *) * (size_t)(*n + 1));
    if (!next) return -1;

    *v = next;
    (*v)[*n] = sys_dup(s);

    if (!(*v)[*n]) return -1;

    (*n)++;
    return 0;
}

int lang_add_ext(LANG *l, const char *ext) {
    return push_str(&l->ext, &l->next, ext);
}

int lang_add(const char *ext, const char *cmd,
             const char *const *args, int nargs,
             const char *outext, int cflags, int fmt) {
    if (g_n >= LANG_MAX) return -1;

    LANG *l = &g_langs[g_n];
    memset(l, 0, sizeof(*l));

    l->cmd    = sys_dup(cmd);
    l->outext = sys_dup(outext ? outext : ".o");
    l->cflags = cflags;
    l->fmt    = fmt;

    if (lang_add_ext(l, ext) != 0) return -1;

    for (int i = 0; i < nargs; i++)
        if (push_str(&l->args, &l->nargs, args[i]) != 0) return -1;

    g_n++;
    return 0;
}

int lang_init_builtin(void) {
    static int done = 0;

    if (done) return 0;

    int n = (int)(sizeof(g_builtin) / sizeof(g_builtin[0]));

    for (int i = 0; i < n; i++)
        lang_add(g_builtin[i].ext, g_builtin[i].cmd,
                 g_builtin[i].args, g_builtin[i].nargs,
                 g_builtin[i].outext, g_builtin[i].cflags, g_builtin[i].fmt);

    done = 1;
    return 0;
}

int lang_load_toml(const TOML *t) {
    char **names = NULL;
    int    n = toml_sections(t, "lang.", &names);

    for (int i = 0; i < n; i++) {
        char sect[256];
        snprintf(sect, sizeof(sect), "lang.%s", names[i]);

        const char *cmd = toml_str(t, sect, "cmd");

        if (!cmd) continue;

        const char *ext = toml_str(t, sect, "ext");
        const char *out = toml_str(t, sect, "out");

        char *args[ARGS_MAX];
        int   nargs = 0;

        for (int k = 0; k < ARGS_MAX; k++) {
            const char *a = toml_arr(t, sect, "args", k);

            if (!a) break;

            args[nargs++] = (char *)a;
        }

        lang_add(ext ? ext : names[i], cmd,
                 (const char *const *)args, nargs,
                 out ? out : ".o",
                 toml_bool(t, sect, "cflags", 0),
                 toml_bool(t, sect, "fmt", 0));
    }

    for (int i = 0; i < n; i++) free(names[i]);

    free(names);
    return 0;
}
