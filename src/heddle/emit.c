#include "emit.h"
#include "lang.h"
#include "link.h"
#include "sys.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

static int is_supported(const char *src) {
    return lang_for(src) != NULL;
}

static int have_src(const TARGET *t, const char *path) {
    for (int i = 0; i < t->nsrc; i++)
        if (!strcmp(t->src[i], path)) return 1;

    return 0;
}

static const TARGET *owner_of(const PROJECT *p, const TARGET *skip, const char *path) {
    for (int i = 0; i < p->ntargets; i++) {
        const TARGET *t = &p->targets[i];
        if (t == skip) continue;
        if (have_src(t, path)) return t;
    }

    return NULL;
}

static int cycle_at(const PROJECT *p, int i, int *state, char *err, size_t errsz) {
    if (state[i] == 1) {
        snprintf(err, errsz, "dependency cycle involving target '%s'", p->targets[i].name);
        return -1;
    }

    if (state[i] == 2) return 0;

    state[i]        = 1;
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
                snprintf(err, errsz, "target '%s': unknown dependency '%s'", t->name, t->deps[k]);
                return -1;
            }

            if (d->type == TARGET_EXE && t->type != TARGET_CUSTOM) {
                snprintf(err, errsz, "target '%s': cannot depend on executable '%s'", t->name,
                         d->name);
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
                snprintf(err, errsz, "target '%s': path escapes project '%s'", t->name, src);
                return -1;
            }

            if (!is_supported(src)) {
                snprintf(err, errsz, "target '%s': unsupported source '%s'", t->name, src);
                return -1;
            }

            const TARGET *owner = owner_of(p, t, src);
            if (owner) {
                snprintf(err, errsz, "target '%s': source '%s' also used by '%s'", t->name, src,
                         owner->name);
                return -1;
            }

            SYS_STAT st;
            if (sys_stat(src, &st) != 0) {
                snprintf(err, errsz, "target '%s': missing source '%s'", t->name, src);
                return -1;
            }
        }

        for (int k = 0; k < t->ninc; k++) {
            SYS_STAT st;
            if (sys_stat(t->inc[k], &st) != 0) {
                snprintf(err, errsz, "target '%s': missing include dir '%s'", t->name, t->inc[k]);
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

static void mark_closure(const PROJECT *p, int i, char *want) {
    if (want[i]) return;

    want[i] = 1;
    for (int k = 0; k < p->targets[i].ndeps; k++) {
        int d = target_index(p, p->targets[i].deps[k]);
        if (d >= 0) mark_closure(p, d, want);
    }
}

static char *object_of(const PROJECT *p, const TARGET *t, const char *src) {
    const char *base = strrchr(src, '/');
    base             = base ? base + 1 : src;
    const char *dot  = strrchr(base, '.');
    size_t      stem = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    const LANG *lg   = lang_for(src);
    const char *ext  = lg && lg->outext ? lg->outext : p->tc.objext;
    char        name[1024];
    snprintf(name, sizeof(name), "%.*s_%.*s%s", (int)strlen(t->name), t->name, (int)stem, base,
             ext);
    size_t n   = strlen(p->build_dir) + strlen(name) + 2;
    char  *out = (char *)malloc(n);
    if (out) snprintf(out, n, "%s/%s", p->build_dir, name);

    return out;
}

char *emit_artifact(const PROJECT *p, const TARGET *t) {
    if (t->out) return project_path(p->root, t->out);

    const char *pre = "";
    const char *ext = p->tc.binext;
    if (t->type == TARGET_STATICLIB) {
        pre = "lib";
        ext = p->tc.libext;
    } else if (t->type == TARGET_SHAREDLIB) {
        pre = p->tc.dllpre;
        ext = p->tc.dllext;
    }

    char name[1024];
    snprintf(name, sizeof(name), "%s%s%s", pre, t->name, ext);
    size_t n   = strlen(p->build_dir) + strlen(name) + 2;
    char  *out = (char *)malloc(n);
    if (out) snprintf(out, n, "%s/%s", p->build_dir, name);

    return out;
}

static const char *asm_format(const PROJECT *p) {
    const char *arch = p->pkg.target.arch;
    if (!arch) return "elf";

    int wide = !strcmp(arch, "x86_64") || !strcmp(arch, "aarch64") || !strcmp(arch, "riscv64") ||
               !strcmp(arch, "powerpc");
    if (p->target_prefix && p->target_prefix[0]) return wide ? "elf64" : "elf32";

#if defined(_WIN32)
    return wide ? "win64" : "win32";
#elif defined(__APPLE__)
    return wide ? "macho64" : "macho32";
#else
    return wide ? "elf64" : "elf32";
#endif
}

static void put_incs(const TARGET *t, char *buf, size_t cap, int *len, int msvc) {
    for (int i = 0; i < t->ninc; i++) addf(buf, cap, len, msvc ? " /I%s" : " -I%s", t->inc[i]);
}

static void put_dep_incs(const PROJECT *p, char *buf, size_t cap, int *len, int msvc) {
    for (int i = 0; i < p->pkg.deps.n; i++) {
        const PKG_SPEC *s = &p->pkg.deps.items[i];
        if (!s->store_path) continue;

        char *inc = project_path(s->store_path, "include");
        if (inc) {
            SYS_STAT st;
            if (sys_stat(inc, &st) == 0 && st.is_dir)
                addf(buf, cap, len, msvc ? " /I%s" : " -I%s", inc);
            free(inc);
        }
    }
}

static void put_dep_ldflags(const PROJECT *p, char *buf, size_t cap, int *len) {
    for (int i = 0; i < p->pkg.deps.n; i++) {
        const PKG_SPEC *s = &p->pkg.deps.items[i];
        if (!s->store_path || s->recipe) continue;

        char *lib = project_path(s->store_path, "lib");
        if (lib) {
            SYS_STAT st;
            if (sys_stat(lib, &st) == 0 && st.is_dir) {
                addf(buf, cap, len, " -L%s", lib);
                char *meta = project_path(s->store_path, ".heddle-pkg");
                FILE *f    = meta ? fopen(meta, "r") : NULL;
                if (f) {
                    char line[256];
                    while (fgets(line, sizeof(line), f)) {
                        size_t n = strlen(line);
                        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;

                        if (line[0]) addf(buf, cap, len, " -l%s", line);
                    }

                    fclose(f);
                } else {
                    addf(buf, cap, len, " -l%s", s->name);
                }

                free(meta);
            }

            free(lib);
        }
    }
}

static void subst_arg(const char *arg, const char *src, const char *out, const char *format,
                      const char *root, char *dst, size_t cap) {
    size_t len = 0;
    for (const char *p = arg; *p && len + 1 < cap;) {
        if (!strncmp(p, "{src}", 5)) {
            len += (size_t)snprintf(dst + len, cap - len, "%s", src);
            p += 5;
        } else if (!strncmp(p, "{out}", 5)) {
            len += (size_t)snprintf(dst + len, cap - len, "%s", out);
            p += 5;
        } else if (!strncmp(p, "{format}", 8)) {
            len += (size_t)snprintf(dst + len, cap - len, "%s", format);
            p += 8;
        } else if (!strncmp(p, "{root}", 6)) {
            len += (size_t)snprintf(dst + len, cap - len, "%s", root);
            p += 6;
        } else {
            dst[len++] = *p++;
        }
    }

    dst[len] = 0;
}

static int lang_is_template(const LANG *lg) {
    for (int a = 0; a < lg->nargs; a++)
        if (strstr(lg->args[a], "{src}") || strstr(lg->args[a], "{out}") ||
            strstr(lg->args[a], "{format}") || strstr(lg->args[a], "{root}"))
            return 1;

    return 0;
}

static void put_args(const LANG *lg, const char *src, const char *out, const char *format,
                     const char *root, char *buf, size_t cap, int *len) {
    for (int a = 0; a < lg->nargs; a++) {
        char sub[2048];
        subst_arg(lg->args[a], src, out, format, root, sub, sizeof(sub));
        addf(buf, cap, len, " %s", sub);
    }
}

static int tc_is_msvc(const PROJECT *p) {
    return p->tc.family && !strcmp(p->tc.family, "msvc");
}

static void build_cmd(const PROJECT *p, const TARGET *t, const LANG *lg, const char *src,
                      const char *out, const char *incs, const char *flags, const char *deffmt,
                      char *cmd, size_t cap) {
    const char *fmt  = t->format ? t->format : deffmt;
    int         msvc = tc_is_msvc(p);
    int         len  = 0;
    addf(cmd, cap, &len, "%s", tc_tool(&p->tc, lg->cmd));
    if (lang_is_template(lg)) {
        put_args(lg, src, out, fmt, p->root, cmd, cap, &len);
        return;
    }

    if (msvc) {
        addf(cmd, cap, &len, " /c /nologo");
        if (lg->cflags) addf(cmd, cap, &len, "%s", flags);

        addf(cmd, cap, &len, "%s /I%s /Fo%s %s", incs, p->root, out, src);
        return;
    }

    put_args(lg, src, out, fmt, p->root, cmd, cap, &len);
    if (lg->fmt) addf(cmd, cap, &len, " -f %s", fmt);
    if (lg->cflags) addf(cmd, cap, &len, "%s", flags);

    addf(cmd, cap, &len, "%s -I%s -o %s %s", incs, p->root, out, src);
}

typedef struct {
    char  *cmd;
    char **out;
    int    nout;
    int   *dep;
    int    ndep;
} STEP;

typedef struct {
    STEP *steps;
    int   n;
    int   cap;
} PLAN;

static void plan_close(PLAN *pl) {
    for (int i = 0; i < pl->n; i++) {
        free(pl->steps[i].cmd);
        for (int k = 0; k < pl->steps[i].nout; k++) free(pl->steps[i].out[k]);

        free(pl->steps[i].out);
        free(pl->steps[i].dep);
    }

    free(pl->steps);
    memset(pl, 0, sizeof(*pl));
}

static STEP *plan_add(PLAN *pl, const char *cmd) {
    if (pl->n == pl->cap) {
        int   newcap = pl->cap ? pl->cap * 2 : 64;
        STEP *steps  = (STEP *)realloc(pl->steps, sizeof(STEP) * (size_t)newcap);
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

    st->out             = next;
    st->out[st->nout++] = sys_dup(path);
}

static void step_dep(STEP *st, int id) {
    for (int i = 0; i < st->ndep; i++)
        if (st->dep[i] == id) return;

    int *next = (int *)realloc(st->dep, sizeof(int) * (size_t)(st->ndep + 1));
    if (!next) return;

    st->dep             = next;
    st->dep[st->ndep++] = id;
}

static void target_incs(const PROJECT *p, const TARGET *t, char *buf, size_t cap, int *len) {
    int msvc = tc_is_msvc(p);
    *len     = 0;
    buf[0]   = 0;
    put_incs(t, buf, cap, len, msvc);
    put_dep_incs(p, buf, cap, len, msvc);
}

static void target_flags(const PROJECT *p, const TARGET *t, char *buf, size_t cap, int *len) {
    *len   = 0;
    buf[0] = 0;
    for (int k = 0; k < p->tc.ncflags; k++) addf(buf, cap, len, " %s", p->tc.cflags[k]);

    for (int k = 0; k < t->ncflags; k++) addf(buf, cap, len, " %s", t->cflags[k]);
}

static int is_compile_unit(const TARGET *t) {
    return t->type != TARGET_CUSTOM && t->type != TARGET_RAW;
}

static int emit_objects(const PROJECT *p, PLAN *pl, int **obj_of, const char *want, char *err,
                        size_t errsz) {
    for (int i = 0; i < p->ntargets; i++) {
        if (!want[i]) continue;

        const TARGET *t = &p->targets[i];
        char          incs[8192];
        char          flags[8192];
        int           ilen = 0;
        int           flen = 0;
        target_incs(p, t, incs, sizeof(incs), &ilen);
        target_flags(p, t, flags, sizeof(flags), &flen);
        if (t->type == TARGET_CUSTOM) continue;

        if (t->type == TARGET_RAW) {
            if (t->nsrc != 1) {
                snprintf(err, errsz, "target '%s': raw needs exactly one source", t->name);
                return -1;
            }

            const LANG *lg = lang_for(t->src[0]);
            if (!lg) {
                snprintf(err, errsz, "target '%s': no language for '%s'", t->name, t->src[0]);
                return -1;
            }

            char *bin = emit_artifact(p, t);
            if (!bin) continue;

            char cmd[16384];
            build_cmd(p, t, lg, t->src[0], bin, incs, flags, "bin", cmd, sizeof(cmd));
            STEP *st = plan_add(pl, cmd);
            if (!st) {
                free(bin);
                snprintf(err, errsz, "out of memory");
                return -1;
            }

            step_out(st, bin);
            obj_of[i][0] = pl->n - 1;
            free(bin);
            continue;
        }

        for (int k = 0; k < t->nsrc; k++) {
            char *obj = object_of(p, t, t->src[k]);
            if (!obj) continue;

            const LANG *lg = lang_for(t->src[k]);
            if (!lg) {
                free(obj);
                snprintf(err, errsz, "target '%s': no language for '%s'", t->name, t->src[k]);
                return -1;
            }

            char cmd[16384];
            build_cmd(p, t, lg, t->src[k], obj, incs, flags, asm_format(p), cmd, sizeof(cmd));
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

static const char *g_self = "heddle";

void emit_set_self(const char *argv0) {
    if (argv0 && argv0[0]) g_self = argv0;
}

static const char *ld_ext(const char *family) {
    if (!strcmp(family, "iar")) return ".icf";
    if (!strcmp(family, "armcc")) return ".sct";

    return NULL;
}

static int is_gnu_script(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && (!strcmp(dot, ".ld") || !strcmp(dot, ".lds"));
}

static char *script_for_family(const PROJECT *p, const TARGET *t, PLAN *pl, int *ok, int *step_id) {
    const char *fam = p->tc.family ? p->tc.family : "gnu";
    *ok             = 1;
    *step_id        = -1;

    if (!t->ldscript) return NULL;

    const char *ext = ld_ext(fam);
    if (!ext || !is_gnu_script(t->ldscript)) return sys_dup(t->ldscript);

    char out[4096];
    snprintf(out, sizeof(out), "%s/%s%s", p->build_dir, t->name, ext);
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "'%s' ldconv %s %s '%s'", g_self, t->ldscript, fam, out);
    STEP *st = plan_add(pl, cmd);
    if (!st) {
        *ok = 0;
        return NULL;
    }

    step_out(st, out);
    *step_id = pl->n - 1;
    return sys_dup(out);
}

int emit_graph(const PROJECT *p, const char *target, const char *graph, char *err, size_t errsz) {
    int root = target_index(p, target);
    if (root < 0) {
        snprintf(err, errsz, "unknown target '%s'", target);
        return -1;
    }

    PLAN pl;
    memset(&pl, 0, sizeof(pl));
    int   n    = p->ntargets;
    char *want = (char *)calloc((size_t)n, 1);
    if (!want) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    mark_closure(p, root, want);
    int **obj_of = (int **)calloc((size_t)n, sizeof(int *));
    int  *art    = (int *)malloc(sizeof(int) * (size_t)n);
    if (!obj_of || !art) {
        free(want);
        free(obj_of);
        free(art);
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    for (int i = 0; i < n; i++) {
        obj_of[i] = (int *)malloc(sizeof(int) * (size_t)p->targets[i].nsrc);
        art[i]    = -1;
    }

    if (emit_objects(p, &pl, obj_of, want, err, errsz) != 0) goto fail;

    int *order = (int *)malloc(sizeof(int) * (size_t)n);
    int  no    = 0;
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
        if (!want[i]) continue;

        const TARGET *t = &p->targets[i];
        if (t->type == TARGET_RAW) {
            art[i] = obj_of[i][0];
            continue;
        }

        char *out = emit_artifact(p, t);
        if (!out) continue;

        char cmd[16384];
        int  len   = 0;
        int  sstep = -1;
        cmd[0]     = 0;
        if (t->type == TARGET_CUSTOM) {
#if defined(_WIN32)
            len = snprintf(cmd, sizeof(cmd), "cmd /c \"%s\"", t->cmd);
#else
            len = snprintf(cmd, sizeof(cmd), "sh -c \"%s\"", t->cmd);
#endif

            for (int k = 0; k < t->nsrc; k++) addf(cmd, sizeof(cmd), &len, " %s", t->src[k]);

            addf(cmd, sizeof(cmd), &len, " > %s", out);
        } else if (t->type == TARGET_STATICLIB) {
            if (tc_is_msvc(p)) {
                addf(cmd, sizeof(cmd), &len, "%s /nologo /OUT:%s", p->tc.ar, out);
            } else {
                addf(cmd, sizeof(cmd), &len, "%s rcs %s", p->tc.ar, out);
            }

            for (int k = 0; k < t->nsrc; k++) {
                char *obj = object_of(p, t, t->src[k]);
                if (obj) addf(cmd, sizeof(cmd), &len, " %s", obj);

                free(obj);
            }
        } else {
            char *objs[4096];
            char *libs[4096];
            int   nobj = 0, nlib = 0;
            for (int k = 0; k < t->nsrc && nobj < 4096; k++) {
                char *obj = object_of(p, t, t->src[k]);
                if (obj) objs[nobj++] = obj;
            }

            for (int k = 0; k < t->ndeps && nlib < 4096; k++) {
                int d = target_index(p, t->deps[k]);
                if (d < 0) continue;

                char *lib = emit_artifact(p, &p->targets[d]);
                if (lib) libs[nlib++] = lib;
            }

            char depflags[4096];
            int  dlen   = 0;
            depflags[0] = 0;
            put_dep_ldflags(p, depflags, sizeof(depflags), &dlen);
            if (dlen && depflags[0] == ' ') {
                memmove(depflags, depflags + 1, (size_t)dlen);
                dlen--;
            }

            char *ldflags[1024];
            int   nldf = 0;
            for (int k = 0; k < p->tc.nldflags && nldf < 1024; k++)
                ldflags[nldf++] = p->tc.ldflags[k];
            for (int k = 0; k < t->nldflags && nldf < 1024; k++) ldflags[nldf++] = t->ldflags[k];

            if (dlen) ldflags[nldf++] = depflags;

            int   sok    = 1;
            char *script = script_for_family(p, t, &pl, &sok, &sstep);
            if (!sok) {
                for (int k = 0; k < nobj; k++) free(objs[k]);
                for (int k = 0; k < nlib; k++) free(libs[k]);
                free(out);
                snprintf(err, errsz, "out of memory");
                goto fail;
            }

            LINK_REQ req;
            memset(&req, 0, sizeof(req));
            req.out      = out;
            req.objs     = objs;
            req.nobj     = nobj;
            req.libs     = libs;
            req.nlib     = nlib;
            req.ldflags  = ldflags;
            req.nldf     = nldf;
            req.ldscript = script;
            req.entry    = t->entry;
            req.shared   = t->type == TARGET_SHAREDLIB;
            link_cmd(&p->tc, &req, cmd, sizeof(cmd));
            free(script);
            for (int k = 0; k < nobj; k++) free(objs[k]);
            for (int k = 0; k < nlib; k++) free(libs[k]);
        }

        STEP *st = plan_add(&pl, cmd);
        if (!st) {
            free(out);
            snprintf(err, errsz, "out of memory");
            goto fail;
        }

        if (sstep >= 0) step_dep(st, sstep);

        step_out(st, out);
        free(out);
        art[i] = pl.n - 1;
    }

    free(order);
    for (int i = 0; i < n; i++) {
        if (art[i] < 0) continue;

        STEP *st = &pl.steps[art[i]];
        if (p->targets[i].type != TARGET_RAW)
            for (int k = 0; k < p->targets[i].nsrc; k++) step_dep(st, obj_of[i][k]);

        for (int k = 0; k < p->targets[i].ndeps; k++) {
            int d = target_index(p, p->targets[i].deps[k]);
            if (d >= 0 && art[d] >= 0) step_dep(st, art[d]);
        }
    }

    for (int i = 0; i < pl.n; i++) {
        STEP *st = &pl.steps[i];
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
            for (int k = 0; k < pl.steps[i].ndep; k++) fprintf(f, " %d", pl.steps[i].dep[k]);
        }

        fputc('\n', f);
    }

    fclose(f);
    for (int i = 0; i < n; i++) free(obj_of[i]);

    free(obj_of);
    free(art);
    free(want);
    plan_close(&pl);
    return 0;

fail:
    for (int i = 0; i < n; i++) free(obj_of[i]);

    free(obj_of);
    free(art);
    free(want);
    plan_close(&pl);
    return -1;
}

static void abs_dir(const char *root, char *buf, size_t cap) {
    char cwd[2048];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;

    if (!root || !root[0] || !strcmp(root, ".")) snprintf(buf, cap, "%s", cwd[0] ? cwd : ".");
    else if (root[0] == '/' || (root[0] && root[1] == ':')) snprintf(buf, cap, "%s", root);
    else snprintf(buf, cap, "%s/%s", cwd, root);

    for (char *q = buf; *q; q++)
        if (*q == '\\') *q = '/';
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p == '\r') fputs("\\r", f);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }

    fputc('"', f);
}

int emit_compile_db(const PROJECT *p, const char *target, const char *out, char *err,
                    size_t errsz) {
    int   n    = p->ntargets;
    char *want = (char *)calloc((size_t)n, 1);
    if (!want) {
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    if (target) {
        int root = target_index(p, target);
        if (root < 0) {
            free(want);
            snprintf(err, errsz, "unknown target '%s'", target);
            return -1;
        }

        mark_closure(p, root, want);
    } else {
        for (int i = 0; i < n; i++) want[i] = 1;
    }

    FILE *f = fopen(out, "w");
    if (!f) {
        free(want);
        snprintf(err, errsz, "cannot write %s", out);
        return -1;
    }

    fputs("[\n", f);
    int first = 1;
    for (int i = 0; i < n; i++) {
        const TARGET *t = &p->targets[i];
        if (!want[i] || !is_compile_unit(t)) continue;

        char incs[8192];
        char flags[8192];
        int  ilen = 0;
        int  flen = 0;
        target_incs(p, t, incs, sizeof(incs), &ilen);
        target_flags(p, t, flags, sizeof(flags), &flen);
        for (int k = 0; k < t->nsrc; k++) {
            const LANG *lg = lang_for(t->src[k]);
            if (!lg) continue;

            char *obj = object_of(p, t, t->src[k]);
            if (!obj) continue;

            char cmd[16384];
            build_cmd(p, t, lg, t->src[k], obj, incs, flags, asm_format(p), cmd, sizeof(cmd));
            if (!first) fputs(",\n", f);

            first = 0;
            char dir[4096];
            abs_dir(p->root, dir, sizeof(dir));
            fputs("  {\n    \"directory\": ", f);
            json_str(f, dir);
            fputs(",\n    \"file\": ", f);
            json_str(f, t->src[k]);
            fputs(",\n    \"command\": ", f);
            json_str(f, cmd);
            fputs(",\n    \"output\": ", f);
            json_str(f, obj);
            fputs("\n  }", f);
            free(obj);
        }
    }

    fputs("\n]\n", f);
    fclose(f);
    free(want);
    (void)errsz;
    return 0;
}
