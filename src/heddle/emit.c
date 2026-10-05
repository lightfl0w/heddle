
#include "emit.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *ext_of(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    const char *dot = strrchr(base, '.');
    return dot && dot != base ? dot + 1 : "";
}

static int is_cxx(const char *src) {
    const char *e = ext_of(src);

    return !strcmp(e, "cc") || !strcmp(e, "cpp") || !strcmp(e, "cxx") ||
           !strcmp(e, "c++");
}

static int is_supported(const char *src) {
    const char *e = ext_of(src);

    return !strcmp(e, "c") || is_cxx(src) || !strcmp(e, "S");
}

static const char *compiler_for(const PROJECT *p, const char *src) {
    return is_cxx(src) ? p->tc.cxx : p->tc.cc;
}

static int have_src(const TARGET *t, const char *path) {
    for (int i = 0; i < t->nsrc; i++)
        if (!strcmp(t->src[i], path)) return 1;

    return 0;
}

static const TARGET *owner_of(const PROJECT *p, const TARGET *skip,
                              const char *path) {
    for (int i = 0; i < p->ntargets; i++) {
        const TARGET *t = &p->targets[i];

        if (t == skip) continue;
        if (have_src(t, path)) return t;
    }

    return NULL;
}

static int cycle_at(const PROJECT *p, int i, int *state,
                    char *err, size_t errsz) {
    if (state[i] == 1) {
        snprintf(err, errsz, "dependency cycle involving target '%s'",
                 p->targets[i].name);
        return -1;
    }

    if (state[i] == 2) return 0;

    state[i] = 1;

    const TARGET *t = &p->targets[i];

    for (int k = 0; k < t->ndeps; k++) {
        int d = -1;

        for (int j = 0; j < p->ntargets; j++)
            if (!strcmp(p->targets[j].name, t->deps[k])) d = j;

        if (d >= 0 && cycle_at(p, d, state, err, errsz) != 0) return -1;
    }

    state[i] = 2;
    return 0;
}

int project_check(const PROJECT *p, char *err, size_t errsz) {
    for (int i = 0; i < p->ntargets; i++) {
        const TARGET *t = &p->targets[i];

        for (int k = 0; k < t->ndeps; k++) {
            const TARGET *d = project_target((PROJECT *)p, t->deps[k]);

            if (!d) {
                snprintf(err, errsz, "target '%s': unknown dependency '%s'",
                         t->name, t->deps[k]);
                return -1;
            }

            if (d->type == TARGET_EXE) {
                snprintf(err, errsz,
                         "target '%s': cannot depend on executable '%s'",
                         t->name, d->name);
                return -1;
            }

            if (d == t) {
                snprintf(err, errsz, "target '%s': depends on itself", t->name);
                return -1;
            }
        }

        for (int k = 0; k < t->nsrc; k++) {
            const char *src = t->src[k];

            if (strstr(src, "..")) {
                snprintf(err, errsz, "target '%s': path escapes project '%s'",
                         t->name, src);
                return -1;
            }

            if (!is_supported(src)) {
                snprintf(err, errsz, "target '%s': unsupported source '%s'",
                         t->name, src);
                return -1;
            }

            const TARGET *owner = owner_of(p, t, src);

            if (owner) {
                snprintf(err, errsz,
                         "target '%s': source '%s' also used by '%s'",
                         t->name, src, owner->name);
                return -1;
            }

            SYS_STAT st;

            if (sys_stat(src, &st) != 0) {
                snprintf(err, errsz, "target '%s': missing source '%s'",
                         t->name, src);
                return -1;
            }
        }

        for (int k = 0; k < t->ninc; k++) {
            SYS_STAT st;

            if (sys_stat(t->inc[k], &st) != 0) {
                snprintf(err, errsz, "target '%s': missing include dir '%s'",
                         t->name, t->inc[k]);
                return -1;
            }
        }
    }

    int *state = (int *)calloc((size_t)p->ntargets, sizeof(int));
    if (!state) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    for (int i = 0; i < p->ntargets; i++) {
        if (cycle_at(p, i, state, err, errsz) != 0) {
            free(state);
            return -1;
        }
    }

    free(state);
    return 0;
}

static void addf(char *buf, size_t cap, int *len, const char *fmt, ...) {
    if ((size_t)*len >= cap) return;

    va_list ap;
    va_start(ap, fmt);

    int n = vsnprintf(buf + *len, cap - (size_t)*len, fmt, ap);

    va_end(ap);

    if (n > 0) *len += n;
}

static int target_index(const PROJECT *p, const char *name) {
    for (int i = 0; i < p->ntargets; i++)
        if (!strcmp(p->targets[i].name, name)) return i;

    return -1;
}

static char *object_of(const PROJECT *p, const TARGET *t, const char *src) {
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;

    const char *dot = strrchr(base, '.');
    size_t stem = dot && dot != base ? (size_t)(dot - base) : strlen(base);

    char  name[1024];
    snprintf(name, sizeof(name), "%.*s_%.*s%s",
             (int)strlen(t->name), t->name, (int)stem, base, p->tc.objext);

    size_t n = strlen(p->build_dir) + strlen(name) + 2;
    char  *out = (char *)malloc(n);

    if (out) snprintf(out, n, "%s/%s", p->build_dir, name);

    return out;
}

char *emit_artifact(const PROJECT *p, const TARGET *t) {
    const char *pre = "";
    const char *ext = p->tc.binext;

    if (t->type == TARGET_STATICLIB) {
        pre = "lib";
        ext = p->tc.libext;
    } else if (t->type == TARGET_SHAREDLIB) {
        pre = p->tc.dllpre;
        ext = p->tc.dllext;
    }

    char   name[1024];
    snprintf(name, sizeof(name), "%s%s%s", pre, t->name, ext);

    size_t n = strlen(p->build_dir) + strlen(name) + 2;
    char  *out = (char *)malloc(n);

    if (out) snprintf(out, n, "%s/%s", p->build_dir, name);

    return out;
}

static void put_flags(const PROJECT *p, const TARGET *t,
                      char *buf, size_t cap, int *len) {
    for (int i = 0; i < t->ninc; i++)
        addf(buf, cap, len, " -I%s", t->inc[i]);

    for (int i = 0; i < p->tc.ncflags; i++)
        addf(buf, cap, len, " %s", p->tc.cflags[i]);

    for (int i = 0; i < t->ncflags; i++)
        addf(buf, cap, len, " %s", t->cflags[i]);
}


typedef struct {
    char  *cmd;
    char **out;
    int    nout;

    int *dep;
    int  ndep;
} STEP;

typedef struct {
    STEP *steps;
    int   n;
    int   cap;
} PLAN;

static void plan_close(PLAN *pl) {
    for (int i = 0; i < pl->n; i++) {
        free(pl->steps[i].cmd);

        for (int k = 0; k < pl->steps[i].nout; k++)
            free(pl->steps[i].out[k]);

        free(pl->steps[i].out);
        free(pl->steps[i].dep);
    }

    free(pl->steps);
    memset(pl, 0, sizeof(*pl));
}

static STEP *plan_add(PLAN *pl, const char *cmd) {
    if (pl->n == pl->cap) {
        int newcap = pl->cap ? pl->cap * 2 : 64;

        STEP *steps = (STEP *)realloc(pl->steps, sizeof(STEP) * (size_t)newcap);
        if (!steps) return NULL;

        pl->steps = steps;
        pl->cap   = newcap;
    }

    STEP *st = &pl->steps[pl->n++];
    memset(st, 0, sizeof(*st));

    st->cmd = sys_dup(cmd);
    return st->cmd ? st : NULL;
}

static void step_out(STEP *st, const char *path) {
    char **next = (char **)realloc(st->out, sizeof(char *) * (size_t)(st->nout + 1));
    if (!next) return;

    st->out = next;
    st->out[st->nout++] = sys_dup(path);
}

static void step_dep(STEP *st, int id) {
    for (int i = 0; i < st->ndep; i++)
        if (st->dep[i] == id) return;

    int *next = (int *)realloc(st->dep, sizeof(int) * (size_t)(st->ndep + 1));
    if (!next) return;

    st->dep = next;
    st->dep[st->ndep++] = id;
}



static int emit_objects(const PROJECT *p, PLAN *pl, int **obj_of,
                        char *err, size_t errsz) {
    for (int i = 0; i < p->ntargets; i++) {
        const TARGET *t = &p->targets[i];

        char flags[8192];
        int  flen = 0;
        flags[0] = 0;

        put_flags(p, t, flags, sizeof(flags), &flen);

        for (int k = 0; k < t->nsrc; k++) {
            char *obj = object_of(p, t, t->src[k]);
            if (!obj) continue;

            char cmd[16384];
            snprintf(cmd, sizeof(cmd), "%s%s -c %s -o %s",
                     compiler_for(p, t->src[k]), flags, t->src[k], obj);

            STEP *st = plan_add(pl, cmd);

            if (!st) {
                free(obj);
                snprintf(err, errsz, "out of memory");
                return -1;
            }

            step_out(st, obj);

            obj_of[i][k] = pl->n - 1;
            free(obj);
        }
    }

    return 0;
}

int emit_graph(const PROJECT *p, const char *target, const char *graph,
               char *err, size_t errsz) {
    int root = target_index(p, target);

    if (root < 0) {
        snprintf(err, errsz, "unknown target '%s'", target);
        return -1;
    }

    PLAN pl;
    memset(&pl, 0, sizeof(pl));

    int n = p->ntargets;

    int **obj_of = (int **)calloc((size_t)n, sizeof(int *));
    int  *art    = (int *)malloc(sizeof(int) * (size_t)n);

    if (!obj_of || !art) {
        free(obj_of);
        free(art);
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    for (int i = 0; i < n; i++) {
        obj_of[i] = (int *)malloc(sizeof(int) * (size_t)p->targets[i].nsrc);
        art[i]    = -1;
    }

    if (emit_objects(p, &pl, obj_of, err, errsz) != 0) goto fail;

    int *order = (int *)malloc(sizeof(int) * (size_t)n);
    int  no = 0;

    if (!order) {
        snprintf(err, errsz, "out of memory");
        goto fail;
    }

    int *done = (int *)calloc((size_t)n, sizeof(int));

    while (no < n) {
        int progressed = 0;

        for (int i = 0; i < n; i++) {
            if (done[i]) continue;

            int ready = 1;

            for (int k = 0; k < p->targets[i].ndeps; k++) {
                int d = target_index(p, p->targets[i].deps[k]);

                if (d >= 0 && !done[d]) ready = 0;
            }

            if (!ready) continue;

            order[no++] = i;
            done[i]     = 1;
            progressed  = 1;
        }

        if (!progressed) break;
    }

    free(done);

    for (int oi = 0; oi < no; oi++) {
        int i = order[oi];

        const TARGET *t = &p->targets[i];

        char *out = emit_artifact(p, t);
        if (!out) continue;

        char cmd[16384];
        int  len = 0;
        cmd[0] = 0;

        if (t->type == TARGET_STATICLIB) {
            addf(cmd, sizeof(cmd), &len, "%s rcs -o %s", p->tc.ar, out);

            for (int k = 0; k < t->nsrc; k++) {
                char *obj = object_of(p, t, t->src[k]);

                if (obj) addf(cmd, sizeof(cmd), &len, " %s", obj);

                free(obj);
            }
        } else {
            addf(cmd, sizeof(cmd), &len, "%s", p->tc.ld);

            if (t->type == TARGET_SHAREDLIB)
                addf(cmd, sizeof(cmd), &len, " %s", p->tc.soflag);

            addf(cmd, sizeof(cmd), &len, " -o %s", out);

            for (int k = 0; k < t->nsrc; k++) {
                char *obj = object_of(p, t, t->src[k]);

                if (obj) addf(cmd, sizeof(cmd), &len, " %s", obj);

                free(obj);
            }

            for (int k = 0; k < t->ndeps; k++) {
                int d = target_index(p, t->deps[k]);

                if (d < 0) continue;

                char *lib = emit_artifact(p, &p->targets[d]);

                if (lib) addf(cmd, sizeof(cmd), &len, " %s", lib);

                free(lib);
            }

            for (int k = 0; k < p->tc.nldflags; k++)
                addf(cmd, sizeof(cmd), &len, " %s", p->tc.ldflags[k]);

            for (int k = 0; k < t->nldflags; k++)
                addf(cmd, sizeof(cmd), &len, " %s", t->ldflags[k]);
        }

        STEP *st = plan_add(&pl, cmd);

        if (!st) {
            free(out);
            snprintf(err, errsz, "out of memory");
            goto fail;
        }

        step_out(st, out);
        free(out);

        art[i] = pl.n - 1;
    }

    free(order);

    for (int i = 0; i < n; i++) {
        if (art[i] < 0) continue;

        STEP *st = &pl.steps[art[i]];

        for (int k = 0; k < p->targets[i].nsrc; k++)
            step_dep(st, obj_of[i][k]);

        for (int k = 0; k < p->targets[i].ndeps; k++) {
            int d = target_index(p, p->targets[i].deps[k]);

            if (d >= 0 && art[d] >= 0) step_dep(st, art[d]);
        }
    }

    for (int i = 0; i < pl.n; i++) {
        STEP *st = &pl.steps[i];
        if (st->ndep > 0) continue;

        for (int k = 0; k < st->nout; k++) {
            for (int j = 0; j < i; j++) {
                for (int m = 0; m < pl.steps[j].nout; m++) {
                    if (strcmp(pl.steps[j].out[m], st->out[k])) continue;

                    step_dep(st, j);
                }
            }
        }
    }

    FILE *f = fopen(graph, "w");

    if (!f) {
        snprintf(err, errsz, "cannot write %s", graph);
        goto fail;
    }

    for (int i = 0; i < pl.n; i++) {
        fprintf(f, "%d: %s", i, pl.steps[i].cmd);

        if (pl.steps[i].ndep > 0) {
            fputs(" <", f);

            for (int k = 0; k < pl.steps[i].ndep; k++)
                fprintf(f, " %d", pl.steps[i].dep[k]);
        }

        fputc('\n', f);
    }

    fclose(f);

    for (int i = 0; i < n; i++) free(obj_of[i]);

    free(obj_of);
    free(art);
    plan_close(&pl);
    return 0;

fail:
    for (int i = 0; i < n; i++) free(obj_of[i]);

    free(obj_of);
    free(art);
    plan_close(&pl);
    return -1;
}
