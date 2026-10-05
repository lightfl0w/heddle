#include "recipe.h"

#include "sys.h"
#include "toml.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *join(const char *a, const char *b) {
    if (!a || !a[0] || !strcmp(a, ".")) return sys_dup(b);

    size_t n = strlen(a) + strlen(b) + 2;
    char  *p = (char *)malloc(n);

    if (p) snprintf(p, n, "%s/%s", a, b);

    return p;
}

static char *dup_opt(const char *s) {
    return s ? sys_dup(s) : NULL;
}

static int exists(const char *p) {
    SYS_STAT st;
    return sys_stat(p, &st) == 0;
}

static void push(char ***v, int *n, const char *s) {
    char **next = (char **)realloc(*v, sizeof(char *) * (size_t)(*n + 1));
    if (!next) return;

    *v = next;
    (*v)[*n] = sys_dup(s);

    if ((*v)[*n]) (*n)++;
}

static int wild(const char *pat, const char *name) {
    const char *star = strchr(pat, '*');

    if (!star) return !strcmp(pat, name);

    size_t pre  = (size_t)(star - pat);
    size_t post = strlen(star + 1);
    size_t ln   = strlen(name);

    if (ln < pre + post) return 0;
    if (pre && strncmp(pat, name, pre)) return 0;
    if (post && strcmp(name + ln - post, star + 1)) return 0;

    return 1;
}

static void scan(RECIPE *r, const char *dir, const char *pat) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char *full = join(dir, e->d_name);
        if (!full) continue;

        if (sys_isdir(full)) {
            if (strchr(pat, '/')) {
                const char *slash = strchr(pat, '/');
                char        head[256];

                snprintf(head, sizeof(head), "%.*s", (int)(slash - pat), pat);

                if (wild(head, e->d_name)) scan(r, full, slash + 1);
            } else {
                scan(r, full, pat);
            }
        } else if (wild(pat, e->d_name)) {
            push(&r->build.files, &r->build.nfile, full);
        }

        free(full);
    }

    closedir(d);
}

int recipe_match(const char *dir) {
    char *cfg = join(dir, "package.toml");
    int   ok  = cfg && exists(cfg);

    free(cfg);
    return ok;
}

static void load_list(const TOML *t, const char *sect, const char *key,
                      char ***out, int *n) {
    for (int i = 0; ; i++) {
        const char *v = toml_arr(t, sect, key, i);

        if (!v) break;

        push(out, n, v);
    }
}

int recipe_load(RECIPE *r, const char *dir, char *err, size_t errsz) {
    memset(r, 0, sizeof(*r));

    r->dir = sys_dup(dir);

    char *cfg = join(dir, "package.toml");

    if (!cfg || !exists(cfg)) {
        snprintf(err, errsz, "cannot read %s", cfg ? cfg : "package.toml");
        free(cfg);
        return -1;
    }

    TOML t;
    toml_init(&t);

    if (toml_parse(&t, cfg, err, errsz) != 0) {
        toml_free(&t);
        free(cfg);
        return -1;
    }

    r->pkg.name    = dup_opt(toml_str(&t, "package", "name"));
    r->pkg.version = dup_opt(toml_str(&t, "package", "version"));

    r->source.url  = dup_opt(toml_str(&t, "source", "url"));
    r->source.tag  = dup_opt(toml_str(&t, "source", "tag"));
    r->source.path = dup_opt(toml_str(&t, "source", "path"));

    r->build.type = dup_opt(toml_str(&t, "build", "type"));

    load_list(&t, "build", "sources",      &r->build.patterns, &r->build.npat);
    load_list(&t, "build", "include_dirs", &r->build.include_dirs, &r->build.ninc);
    load_list(&t, "build", "defines",      &r->build.defines, &r->build.ndef);
    load_list(&t, "build", "cflags",       &r->build.cflags, &r->build.ncflags);

    toml_free(&t);
    free(cfg);

    if (!r->pkg.name) {
        snprintf(err, errsz, "%s: missing [package] name", dir);
        recipe_free(r);
        return -1;
    }

    for (int i = 0; i < r->build.npat; i++)
        scan(r, r->dir, r->build.patterns[i]);

    if (r->build.npat && !r->build.nfile) {
        snprintf(err, errsz, "%s: no sources match", dir);
        recipe_free(r);
        return -1;
    }

    return 0;
}

void recipe_free(RECIPE *r) {
    free(r->pkg.name);
    free(r->pkg.version);

    free(r->source.url);
    free(r->source.tag);
    free(r->source.path);

    for (int i = 0; i < r->build.npat; i++)    free(r->build.patterns[i]);
    for (int i = 0; i < r->build.nfile; i++)   free(r->build.files[i]);
    for (int i = 0; i < r->build.ninc; i++)    free(r->build.include_dirs[i]);
    for (int i = 0; i < r->build.ndef; i++)    free(r->build.defines[i]);
    for (int i = 0; i < r->build.ncflags; i++) free(r->build.cflags[i]);

    free(r->build.patterns);
    free(r->build.files);
    free(r->build.include_dirs);
    free(r->build.defines);
    free(r->build.cflags);
    free(r->build.type);

    free(r->dir);
    memset(r, 0, sizeof(*r));
}
