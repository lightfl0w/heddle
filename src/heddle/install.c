#include "install.h"

#include "glob.h"
#include "hash.h"
#include "sys.h"
#include "toml.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_opt(const char *s) {
    return s ? sys_dup(s) : NULL;
}

static char *join(const char *a, const char *b) {
    if (!a || !a[0] || !strcmp(a, ".")) return sys_dup(b);

    size_t al = strlen(a);

    while (al > 1 && a[al - 1] == '/') al--;

    size_t bl = strlen(b);

    while (bl && b[0] == '/') { b++; bl--; }

    size_t n = al + bl + 2;
    char  *p = (char *)malloc(n);

    if (p) snprintf(p, n, "%.*s/%s", (int)al, a, b);

    return p;
}

static char *join3(const char *a, const char *b, const char *c) {
    char *ab = join(a, b);
    char *r  = ab ? join(ab, c) : NULL;

    free(ab);
    return r;
}

static int exists(const char *p) {
    SYS_STAT st;
    return sys_stat(p, &st) == 0;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');

    return s ? s + 1 : p;
}

static void push(char ***v, int *n, const char *s) {
    char **next = (char **)realloc(*v, sizeof(char *) * (size_t)(*n + 1));
    if (!next) return;

    *v = next;
    (*v)[*n] = sys_dup(s);

    if ((*v)[*n]) (*n)++;
}

static void list_free(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);

    free(v);
}

static void load_key(const TOML *t, const char *sect, const char *key,
                     const char *root, char ***out, int *n) {
    const char *one = toml_str(t, sect, key);

    if (one) {
        char *full = join(root, one);

        if (full) { push(out, n, full); free(full); }
    }

    for (int i = 0; ; i++) {
        const char *v = toml_arr(t, sect, key, i);

        if (!v) break;

        char *full = join(root, v);

        if (full) { push(out, n, full); free(full); }
    }
}

static INSTALL_RULE *rule_add(INSTALL_SET *s, const char *name) {
    if (s->n == s->cap) {
        int newcap = s->cap ? s->cap * 2 : 8;

        INSTALL_RULE *rules = (INSTALL_RULE *)realloc(
            s->rules, sizeof(INSTALL_RULE) * (size_t)newcap);
        if (!rules) return NULL;

        s->rules = rules;
        s->cap   = newcap;
    }

    INSTALL_RULE *r = &s->rules[s->n++];
    memset(r, 0, sizeof(*r));

    r->name = sys_dup(name);
    return r;
}

int install_load(INSTALL_SET *s, const PROJECT *p, char *err, size_t errsz) {
    char cfg[4096];
    snprintf(cfg, sizeof(cfg), "%s/heddle.toml", p->root);

    TOML t;
    toml_init(&t);

    if (toml_parse(&t, cfg, err, errsz) != 0) {
        toml_free(&t);
        return -1;
    }

    char **names = NULL;
    int    n     = toml_sections(&t, "target.", &names);

    for (int i = 0; i < n; i++) {
        char sect[1024];
        snprintf(sect, sizeof(sect), "target.%s.install", names[i]);

        const char *rootfs = toml_str(&t, sect, "rootfs");
        int has = rootfs != NULL;

        if (!has) {
            const char *keys[] = { "bin", "lib", "include", "share", "etc",
                                   "sysroot_lib", "sysroot_include" };

            for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
                if (toml_str(&t, sect, keys[k]) ||
                    toml_arr(&t, sect, keys[k], 0))
                    has = 1;
        }

        if (!has) continue;

        INSTALL_RULE *r = rule_add(s, names[i]);

        if (!r) {
            snprintf(err, errsz, "out of memory");
            goto fail;
        }

        load_key(&t, sect, "bin",   p->root, &r->bin,   &r->nbin);
        load_key(&t, sect, "lib",   p->root, &r->lib,   &r->nlib);
        load_key(&t, sect, "include", p->root, &r->include, &r->ninc);
        load_key(&t, sect, "share", p->root, &r->share, &r->nshare);
        load_key(&t, sect, "etc",   p->root, &r->etc,   &r->netc);
        load_key(&t, sect, "sysroot_lib", p->root, &r->sysroot_lib, &r->nsrlib);
        load_key(&t, sect, "sysroot_include", p->root,
                 &r->sysroot_include, &r->nsrinc);

        r->rootfs = dup_opt(rootfs);
    }

    for (int i = 0; i < n; i++) free(names[i]);

    free(names);
    toml_free(&t);
    return 0;

fail:
    for (int i = 0; i < n; i++) free(names[i]);

    free(names);
    toml_free(&t);
    return -1;
}

void install_free(INSTALL_SET *s) {
    for (int i = 0; i < s->n; i++) {
        INSTALL_RULE *r = &s->rules[i];

        free(r->name);
        list_free(r->bin, r->nbin);
        list_free(r->lib, r->nlib);
        list_free(r->include, r->ninc);
        list_free(r->share, r->nshare);
        list_free(r->etc, r->netc);
        list_free(r->sysroot_lib, r->nsrlib);
        list_free(r->sysroot_include, r->nsrinc);
        free(r->rootfs);
    }

    free(s->rules);
    memset(s, 0, sizeof(*s));
}

INSTALL_RULE *install_rule(INSTALL_SET *s, const char *target) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->rules[i].name, target)) return &s->rules[i];

    return NULL;
}

int install_has_any(const INSTALL_SET *s) {
    return s->n > 0;
}

static void plan_push(INSTALL_PLAN *pl, const char *src, const char *dst) {
    if (pl->n == pl->cap) {
        int newcap = pl->cap ? pl->cap * 2 : 16;

        INSTALL_ITEM *items = (INSTALL_ITEM *)realloc(
            pl->items, sizeof(INSTALL_ITEM) * (size_t)newcap);
        if (!items) return;

        pl->items = items;
        pl->cap   = newcap;
    }

    pl->items[pl->n].src = sys_dup(src);
    pl->items[pl->n].dst = sys_dup(dst);
    pl->n++;
}

void install_plan_free(INSTALL_PLAN *pl) {
    for (int i = 0; i < pl->n; i++) {
        free(pl->items[i].src);
        free(pl->items[i].dst);
    }

    free(pl->items);
    memset(pl, 0, sizeof(*pl));
}

typedef struct {
    char *path;
    char *base;
} EXPAND_ITEM;

static int cmp_expand(const void *a, const void *b) {
    const EXPAND_ITEM *x = (const EXPAND_ITEM *)a;
    const EXPAND_ITEM *y = (const EXPAND_ITEM *)b;

    return strcmp(x->path, y->path);
}

static int glob_expand_list(char **pats, int np, EXPAND_ITEM **out, int *n,
                            char *err, size_t errsz) {
    for (int i = 0; i < np; i++) {
        const char *pat = pats[i];

        char        dir[4096];
        const char *glob_pat = pat;

        const char *star = strpbrk(pat, "*?");

        if (star) {
            const char *slash = NULL;

            for (const char *q = pat; q < star; q++)
                if (*q == '/') slash = q;

            if (slash) {
                size_t dn = (size_t)(slash - pat);

                if (dn >= sizeof(dir)) {
                    snprintf(err, errsz, "path too long: %s", pat);
                    return -1;
                }

                memcpy(dir, pat, dn);
                dir[dn] = 0;
                glob_pat = slash + 1;
            } else {
                snprintf(dir, sizeof(dir), ".");
            }

            GLOB_LIST l;
            glob_init(&l);

            if (glob_dir(dir, glob_pat, &l) == 0) {
                snprintf(err, errsz, "install pattern matches nothing: %s", pat);
                glob_free(&l);
                return -1;
            }

            for (int k = 0; k < l.n; k++) {
                char *full = join(dir, l.items[k]);

                if (!full) continue;

                EXPAND_ITEM *np2 = (EXPAND_ITEM *)realloc(
                    *out, sizeof(EXPAND_ITEM) * (size_t)(*n + 1));
                if (!np2) { free(full); glob_free(&l); return -1; }

                *out = np2;
                (*out)[*n].path = full;
                (*out)[*n].base = sys_dup(dir);
                (*n)++;
            }

            glob_free(&l);
        } else {
            char *full = sys_dup(pat);

            if (!full) {
                snprintf(err, errsz, "out of memory");
                return -1;
            }

            EXPAND_ITEM *np2 = (EXPAND_ITEM *)realloc(
                *out, sizeof(EXPAND_ITEM) * (size_t)(*n + 1));
            if (!np2) { free(full); snprintf(err, errsz, "out of memory"); return -1; }

            *out = np2;
            (*out)[*n].path = full;
            (*out)[*n].base = sys_dup("");
            (*n)++;
        }
    }

    if (*n > 1) qsort(*out, (size_t)*n, sizeof(EXPAND_ITEM), cmp_expand);

    return 0;
}

static char *dest_join(const char *destdir, const char *prefix, const char *sub,
                       const char *leaf) {
    const char *pfx = prefix;

    if (destdir && destdir[0])
        while (*pfx == '/') pfx++;

    char *root = join(destdir && destdir[0] ? destdir : "", pfx);
    char *full = join3(root, sub, leaf);

    free(root);
    return full;
}

static char *dest_join_abs(const char *destdir, const char *absroot,
                           const char *sub, const char *leaf);

static int add_group(const char *destdir, const char *prefix,
                     const char *root_abs, INSTALL_PLAN *pl, char **pats, int np,
                     const char *sub, int flatten, char *err, size_t errsz) {
    if (!np) return 0;

    EXPAND_ITEM *items = NULL;
    int          nf    = 0;

    if (glob_expand_list(pats, np, &items, &nf, err, errsz) != 0) {
        for (int i = 0; i < nf; i++) { free(items[i].path); free(items[i].base); }

        free(items);
        return -1;
    }

    int rc = 0;

    for (int i = 0; i < nf && rc == 0; i++) {
        const char *src = items[i].path;

        if (!exists(src)) {
            snprintf(err, errsz, "install source missing: %s", src);
            rc = -1;
            break;
        }

        char leaf[4096];

        if (flatten) {
            snprintf(leaf, sizeof(leaf), "%s", base_name(src));
        } else {
            const char *b  = items[i].base;
            size_t      bl = strlen(b);
            const char *rel = src;

            if (bl && !strncmp(src, b, bl)) {
                rel = src + bl;

                while (*rel == '/') rel++;
            }

            snprintf(leaf, sizeof(leaf), "%s", *rel ? rel : base_name(src));
        }

        char *dst = root_abs
                        ? dest_join_abs(destdir, root_abs, sub, leaf)
                        : dest_join(destdir, prefix, sub, leaf);

        if (!dst) {
            snprintf(err, errsz, "out of memory");
            rc = -1;
            break;
        }

        plan_push(pl, src, dst);
        free(dst);
    }

    for (int i = 0; i < nf; i++) { free(items[i].path); free(items[i].base); }

    free(items);
    return rc;
}

static char *dest_join_abs(const char *destdir, const char *absroot,
                           const char *sub, const char *leaf) {
    const char *root = absroot;

    if (destdir && destdir[0])
        while (*root == '/') root++;

    char *a = join(destdir && destdir[0] ? destdir : "", root);
    char *b = join3(a, sub, leaf);

    free(a);
    return b;
}

static int add_tree(INSTALL_PLAN *pl, const char *src, const char *dstroot,
                    const char *rel, char *err, size_t errsz) {
    DIR *d = opendir(src);

    if (!d) {
        snprintf(err, errsz, "cannot read %s", src);
        return -1;
    }

    struct dirent *e;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char *full = join(src, e->d_name);
        if (!full) { closedir(d); return -1; }

        char rel2[4096];
        snprintf(rel2, sizeof(rel2), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);

        SYS_STAT st;
        int rc = 0;

        if (sys_stat(full, &st) == 0 && st.is_dir) {
            rc = add_tree(pl, full, dstroot, rel2, err, errsz);
        } else {
            char *dst = join(dstroot, rel2);

            if (!dst) rc = -1;
            else {
                plan_push(pl, full, dst);
                free(dst);
            }
        }

        free(full);

        if (rc != 0) { closedir(d); return rc; }
    }

    closedir(d);
    return 0;
}

int install_plan(INSTALL_SET *s, PROJECT *p, const char *target,
                 const char *prefix, const char *destdir,
                 INSTALL_PLAN *out, char *err, size_t errsz) {
    for (int i = 0; i < s->n; i++) {
        INSTALL_RULE *r = &s->rules[i];

        if (target && strcmp(target, r->name)) continue;

        if (add_group(destdir, prefix, NULL, out, r->bin, r->nbin, "bin", 1,
                      err, errsz) != 0)
            return -1;

        if (add_group(destdir, prefix, NULL, out, r->lib, r->nlib, "lib", 1,
                      err, errsz) != 0)
            return -1;

        if (add_group(destdir, prefix, NULL, out, r->include, r->ninc, "include",
                      0, err, errsz) != 0)
            return -1;

        char sub[4096];

        snprintf(sub, sizeof(sub), "share/%s", r->name);

        if (add_group(destdir, prefix, NULL, out, r->share, r->nshare, sub, 1,
                      err, errsz) != 0)
            return -1;

        snprintf(sub, sizeof(sub), "etc/%s", r->name);

        if (add_group(destdir, prefix, NULL, out, r->etc, r->netc, sub, 1,
                      err, errsz) != 0)
            return -1;

        const char *sysroot = p->pkg.sysroot && p->pkg.sysroot[0]
                                  ? p->pkg.sysroot : p->target_sysroot;

        if ((r->nsrlib || r->nsrinc) && (!sysroot || !sysroot[0])) {
            snprintf(err, errsz,
                     "target '%s': sysroot_* install needs [target] sysroot",
                     r->name);
            return -1;
        }

        if (r->nsrlib &&
            add_group(destdir, "", sysroot, out, r->sysroot_lib, r->nsrlib,
                      "lib", 1, err, errsz) != 0)
            return -1;

        if (r->nsrinc &&
            add_group(destdir, "", sysroot, out, r->sysroot_include, r->nsrinc,
                      "include", 0, err, errsz) != 0)
            return -1;

        if (r->rootfs) {
            char *src   = join(p->root, r->rootfs);
            char *dstroot = dest_join_abs(destdir, prefix, "rootfs", "");

            if (!src || !dstroot) {
                free(src);
                free(dstroot);
                snprintf(err, errsz, "out of memory");
                return -1;
            }

            int rc = add_tree(out, src, dstroot, "", err, errsz);

            free(src);
            free(dstroot);

            if (rc != 0) return -1;
        }
    }

    return 0;
}

int install_dry_run(const INSTALL_PLAN *pl) {
    if (!pl->n) {
        printf("heddle: nothing to install\n");
        return 0;
    }

    printf("heddle: install plan (%d file%s)\n", pl->n, pl->n == 1 ? "" : "s");

    for (int i = 0; i < pl->n; i++)
        printf("  %-40s -> %s\n", pl->items[i].src, pl->items[i].dst);

    return 0;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;

    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char   buf[65536];
    size_t got;
    int    rc = 0;

    while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, got, out) != got) { rc = -1; break; }

    fclose(in);
    if (fclose(out) != 0) rc = -1;

    return rc;
}

static void dir_of(const char *path, char *buf, size_t cap) {
    snprintf(buf, cap, "%s", path);

    char *slash = strrchr(buf, '/');

    if (slash) *slash = 0;
}

void install_log_path(char *buf, size_t cap, const char *root,
                      const char *destdir, const char *prefix) {
    unsigned long long h = HASH_FNV_OFFSET;

    h = hash_text(h, destdir ? destdir : "");
    h = hash_text(h, prefix);

    snprintf(buf, cap, "%s/.heddle/install/%016llx.log", root, h);
}

int install_write_log(const char *log, const INSTALL_PLAN *pl,
                      char *err, size_t errsz) {
    char dir[4096];
    dir_of(log, dir, sizeof(dir));
    sys_mkpath(dir);

    FILE *f = fopen(log, "w");

    if (!f) {
        snprintf(err, errsz, "cannot write %s", log);
        return -1;
    }

    fputs("# generated by heddle install; do not edit\n", f);

    for (int i = 0; i < pl->n; i++)
        fprintf(f, "%s\t%s\n", pl->items[i].dst, pl->items[i].src);

    fclose(f);
    return 0;
}

int install_run(INSTALL_PLAN *pl, const char *log,
                int verbose, char *err, size_t errsz) {
    for (int i = 0; i < pl->n; i++) {
        const char *src = pl->items[i].src;
        const char *dst = pl->items[i].dst;

        char dir[4096];
        dir_of(dst, dir, sizeof(dir));
        sys_mkpath(dir);

        if (copy_file(src, dst) != 0) {
            snprintf(err, errsz, "cannot install %s -> %s", src, dst);
            return -1;
        }

        SYS_STAT st;

        if (sys_stat(src, &st) == 0 && (st.mode & 0111)) sys_chmod(dst, 0755);

        if (verbose) fprintf(stderr, "heddle: install %s -> %s\n", src, dst);
    }

    if (install_write_log(log, pl, err, errsz) != 0) return -1;

    return 0;
}

int install_uninstall(const char *log, int verbose, char *err, size_t errsz) {
    FILE *f = fopen(log, "r");

    if (!f) {
        snprintf(err, errsz, "no install log at %s", log);
        return -1;
    }

    char  line[8192];
    int   removed = 0;
    int   missing = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;

        char *tab = strchr(line, '\t');

        if (!tab) continue;

        *tab = 0;

        char *dst = line;

        if (remove(dst) == 0) {
            removed++;
            if (verbose) fprintf(stderr, "heddle: remove %s\n", dst);
        } else {
            missing++;
        }
    }

    fclose(f);
    remove(log);

    printf("heddle: removed %d file%s (%d already gone)\n", removed,
           removed == 1 ? "" : "s", missing);

    (void)err;
    (void)errsz;
    return 0;
}
