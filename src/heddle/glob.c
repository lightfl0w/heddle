#include "glob.h"

#include "sys.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void glob_init(GLOB_LIST *l) {
    memset(l, 0, sizeof(*l));
}

void glob_free(GLOB_LIST *l) {
    for (int i = 0; i < l->n; i++) free(l->items[i]);

    free(l->items);
    memset(l, 0, sizeof(*l));
}

int glob_has_wild(const char *s) {
    return strpbrk(s, "*?") != NULL;
}

static int seg_match(const char *pat, const char *name) {
    if (!*pat) return !*name;

    if (*pat == '*') {
        if (seg_match(pat + 1, name)) return 1;

        return *name && seg_match(pat, name + 1);
    }

    if (*pat == '?') return *name && seg_match(pat + 1, name + 1);

    return *pat == *name && seg_match(pat + 1, name + 1);
}

static int match_segs(const char *pat, const char *name) {
    if (!*pat) return !*name;

    const char *ps = strchr(pat, '/');
    const char *ns = strchr(name, '/');
    char        pseg[512], nseg[512];
    size_t      pl = ps ? (size_t)(ps - pat) : strlen(pat);
    size_t      nl = ns ? (size_t)(ns - name) : strlen(name);
    if (pl >= sizeof(pseg) || nl >= sizeof(nseg)) return 0;

    memcpy(pseg, pat, pl);
    pseg[pl] = 0;
    memcpy(nseg, name, nl);
    nseg[nl] = 0;
    int pend = ps == NULL;
    int nend = ns == NULL;
    if (!strcmp(pseg, "**")) {
        const char *prest = pend ? "" : ps + 1;
        if (match_segs(prest, name)) return 1;

        return !nend && match_segs(pat, ns + 1);
    }

    if (!seg_match(pseg, nseg)) return 0;

    if (pend) return nend;
    if (nend) return 0;

    return match_segs(ps + 1, ns + 1);
}

int glob_match(const char *pat, const char *name) {
    return match_segs(pat, name);
}

static char *join(const char *a, const char *b) {
    if (!a || !a[0] || !strcmp(a, ".")) return sys_dup(b);

    size_t n = strlen(a) + strlen(b) + 2;
    char  *p = (char *)malloc(n);
    if (p) snprintf(p, n, "%s/%s", a, b);

    return p;
}

static void push(GLOB_LIST *l, const char *s) {
    if (l->n == l->cap) {
        int    newcap = l->cap ? l->cap * 2 : 16;
        char **items  = (char **)realloc(l->items, sizeof(char *) * (size_t)newcap);
        if (!items) return;

        l->items = items;
        l->cap   = newcap;
    }

    l->items[l->n] = sys_dup(s);
    if (l->items[l->n]) l->n++;
}

static void walk(const char *base, const char *rel, const char *pat, GLOB_LIST *out) {
    char *dir = rel[0] ? join(base, rel) : sys_dup(base);
    DIR  *d   = dir ? opendir(dir) : NULL;
    if (!d) {
        free(dir);
        return;
    }

    struct dirent *e;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char r[4096];
        snprintf(r, sizeof(r), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        char *full = join(dir, e->d_name);
        if (!full) continue;

        if (sys_isdir(full)) walk(base, r, pat, out);
        else if (glob_match(pat, r)) push(out, r);

        free(full);
    }

    closedir(d);
    free(dir);
}

int glob_dir(const char *dir, const char *pat, GLOB_LIST *out) {
    int before = out->n;
    walk(dir, "", pat, out);
    return out->n - before;
}
