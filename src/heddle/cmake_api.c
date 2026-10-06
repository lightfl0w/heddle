#include "migrate.h"

#include "proc.h"
#include "sys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

typedef enum { JNUM, JSTR, JARR, JOBJ } JTYPE;

typedef struct JV JV;

struct JV {
    JTYPE  type;
    char  *str;
    JV   **items;
    int    n;
    char **keys;
    JV   **vals;
    int    nkv;
};

typedef struct {
    const char *p, *end;
    int         depth;
} JPARSE;

static void jv_free(JV *v) {
    if (!v) return;
    free(v->str);
    for (int i = 0; i < v->n; i++) jv_free(v->items[i]);
    for (int i = 0; i < v->nkv; i++) {
        free(v->keys[i]);
        jv_free(v->vals[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->vals);
    free(v);
}

static JV *jv_new(JTYPE t) {
    JV *v = calloc(1, sizeof(JV));
    if (v) v->type = t;
    return v;
}

static void skip_ws(JPARSE *s) {
    while (s->p < s->end && isspace((unsigned char)*s->p)) s->p++;
}

static int hex4(const char *p) {
    int cp = 0;
    for (int i = 0; i < 4; i++) {
        char h = p[i];
        cp <<= 4;
        if (h >= '0' && h <= '9') cp |= h - '0';
        else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
        else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
    }
    return cp;
}

static void put_utf8(char *out, size_t *n, unsigned cp) {
    if (cp < 0x80) {
        out[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*n)++] = (char)(0xC0 | (cp >> 6));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*n)++] = (char)(0xE0 | (cp >> 12));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*n)++] = (char)(0xF0 | (cp >> 18));
        out[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static char *read_str(JPARSE *s) {
    if (s->p >= s->end || *s->p != '"') return NULL;
    s->p++;
    size_t cap = 32, n = 0;
    char  *out = malloc(cap);
    if (!out) return NULL;

    while (s->p < s->end && *s->p != '"') {
        unsigned char c = (unsigned char)*s->p++;
        if (c == '\\' && s->p < s->end) {
            char e = *s->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned cp = (unsigned)hex4(s->p);
                s->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && s->end - s->p >= 6 && s->p[0] == '\\' &&
                    s->p[1] == 'u') {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)hex4(s->p + 2) - 0xDC00);
                    s->p += 6;
                }
                if (n + 4 >= cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                    if (!out) return NULL;
                }
                put_utf8(out, &n, cp);
                continue;
            }
            default: c = (unsigned char)e; break;
            }
        }

        if (n + 1 >= cap) {
            cap *= 2;
            out = realloc(out, cap);
            if (!out) return NULL;
        }
        out[n++] = (char)c;
    }

    if (s->p < s->end) s->p++;
    out[n] = 0;
    return out;
}

static JV *parse_value(JPARSE *s);

static JV *parse_object(JPARSE *s) {
    JV *o = jv_new(JOBJ);
    s->p++;
    for (;;) {
        skip_ws(s);
        if (s->p >= s->end || *s->p == '}') {
            if (s->p < s->end) s->p++;
            break;
        }

        char *key = read_str(s);
        if (!key) break;

        skip_ws(s);
        if (s->p < s->end && *s->p == ':') s->p++;

        char **nk = realloc(o->keys, sizeof(char *) * (size_t)(o->nkv + 1));
        JV   **nv = realloc(o->vals, sizeof(JV *) * (size_t)(o->nkv + 1));
        if (!nk || !nv) {
            free(key);
            break;
        }

        o->keys         = nk;
        o->vals         = nv;
        o->keys[o->nkv] = key;
        o->vals[o->nkv] = parse_value(s);
        o->nkv++;
        skip_ws(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (s->p < s->end) s->p++;
        break;
    }
    return o;
}

static JV *parse_array(JPARSE *s) {
    JV *a = jv_new(JARR);
    s->p++;
    for (;;) {
        skip_ws(s);
        if (s->p >= s->end || *s->p == ']') {
            if (s->p < s->end) s->p++;
            break;
        }

        JV **ni = realloc(a->items, sizeof(JV *) * (size_t)(a->n + 1));
        if (!ni) break;
        a->items         = ni;
        a->items[a->n++] = parse_value(s);
        skip_ws(s);
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue;
        }
        if (s->p < s->end) s->p++;
        break;
    }
    return a;
}

static JV *parse_value(JPARSE *s) {
    skip_ws(s);
    if (s->p >= s->end || s->depth++ > 128) {
        s->depth--;
        return NULL;
    }

    JV *v = NULL;
    switch (*s->p) {
    case '{': v = parse_object(s); break;
    case '[': v = parse_array(s); break;
    case '"':
        v      = jv_new(JSTR);
        v->str = read_str(s);
        break;
    case 't':
        v = jv_new(JNUM);
        s->p += 4;
        break;
    case 'f':
        v = jv_new(JNUM);
        s->p += 5;
        break;
    case 'n':
        v = jv_new(JNUM);
        s->p += 4;
        break;
    default: {
        const char *b = s->p;
        while (s->p < s->end && !strchr(",}] \t\r\n", *s->p)) s->p++;
        if (s->p > b) v = jv_new(JNUM);
        break;
    }
    }

    s->depth--;
    return v;
}

static JV *json_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got]   = 0;
    fclose(f);
    JPARSE s = {buf, buf + got, 0};
    JV    *v = parse_value(&s);
    free(buf);
    return v;
}

static JV *jv_get(const JV *o, const char *key) {
    if (!o || o->type != JOBJ) return NULL;
    for (int i = 0; i < o->nkv; i++)
        if (!strcmp(o->keys[i], key)) return o->vals[i];
    return NULL;
}

static const char *jv_str(const JV *v) {
    return (v && v->type == JSTR) ? v->str : NULL;
}

static void push(char ***v, int *n, const char *s) {
    char **next = realloc(*v, sizeof(char *) * (size_t)(*n + 1));
    if (!next) return;
    *v           = next;
    (*v)[(*n)++] = sys_dup(s);
}

static void push_uniq(char ***v, int *n, const char *s) {
    for (int i = 0; i < *n; i++)
        if (!strcmp((*v)[i], s)) return;
    push(v, n, s);
}

static MIG_TARGET *target_get(MIG_SET *s, const char *name) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->items[i].name, name)) return &s->items[i];
    return NULL;
}

static MIG_TARGET *target_add(MIG_SET *s, const char *name) {
    MIG_TARGET *e = target_get(s, name);
    if (e) return e;

    if (s->n == s->cap) {
        s->cap   = s->cap ? s->cap * 2 : 8;
        s->items = realloc(s->items, sizeof(MIG_TARGET) * (size_t)s->cap);
    }

    MIG_TARGET *t = &s->items[s->n++];
    memset(t, 0, sizeof(*t));
    t->name = sys_dup(name);
    return t;
}

static void warn(MIG_SET *s, const char *msg) {
    push(&s->warn, &s->nwarn, msg);
}

static int is_abs(const char *p) {
    if (p[0] == '/') return 1;
#if defined(_WIN32)
    if (isalpha((unsigned char)p[0]) && p[1] == ':') return 1;
    if (p[0] == '\\') return 1;
#endif
    return 0;
}

static char *join_from(const char *dir, const char *rel) {
    if (!rel) return NULL;
    if (is_abs(rel)) return sys_dup(rel);
    size_t n = strlen(dir) + strlen(rel) + 2;
    char  *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", dir, rel);
    return p;
}

static void mkdirs(const char *path) {
    char tmp[16384];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *q = tmp + 1; *q; q++) {
        if (*q != '/' && *q != '\\') continue;
        char keep = *q;
        *q        = 0;
        sys_mkpath(tmp);
        *q = keep;
    }
    sys_mkpath(tmp);
}

static void write_empty(const char *dir, const char *name) {
    char path[16384];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) fclose(f);
}

static char *find_reply(const char *dir, const char *prefix) {
#if defined(_WIN32)
    char pat[16384];
    snprintf(pat, sizeof(pat), "%s/%s*.json", dir, prefix);
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char *out = join_from(dir, fd.cFileName);
    FindClose(h);
    return out;
#else
    size_t plen = strlen(prefix);
    DIR   *d    = opendir(dir);
    if (!d) return NULL;
    struct dirent *e;
    char          *out = NULL;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (strncmp(e->d_name, prefix, plen) || l < 5 || strcmp(e->d_name + l - 5, ".json"))
            continue;
        out = join_from(dir, e->d_name);
        break;
    }
    closedir(d);
    return out;
#endif
}

static const char *map_type(const char *t) {
    if (t && (!strcmp(t, "STATIC_LIBRARY") || !strcmp(t, "OBJECT_LIBRARY"))) return "staticlib";
    if (t && (!strcmp(t, "SHARED_LIBRARY") || !strcmp(t, "MODULE_LIBRARY"))) return "sharedlib";
    return "exe";
}

static int is_internal(const char *nm, const char *ty) {
    static const char *known[] = {"ALL_BUILD", "ZERO_CHECK", "INSTALL",       "RUN_TESTS",
                                  "PACKAGE",   "edit_cache", "rebuild_cache", NULL};
    if (ty && !strcmp(ty, "UTILITY")) return 1;
    if (!nm) return 0;
    for (int i = 0; known[i]; i++)
        if (!strcmp(nm, known[i])) return 1;
    return !strncmp(nm, "Nightly", 7) || !strncmp(nm, "Continuous", 10) ||
           !strncmp(nm, "Experimental", 12);
}

static void id_to_name(const char *id, char *out, size_t cap) {
    const char *c = id ? strstr(id, "::") : NULL;
    size_t      n = c ? (size_t)(c - id) : (id ? strlen(id) : 0);
    if (n >= cap) n = cap - 1;
    if (id) memcpy(out, id, n);
    out[n] = 0;
}

static void add_sources(MIG_TARGET *t, const JV *j, const char *src) {
    JV *a = jv_get(j, "sources");
    if (!a || a->type != JARR) return;
    for (int i = 0; i < a->n; i++) {
        const char *p = jv_str(jv_get(a->items[i], "path"));
        if (!p) continue;
        char *full = join_from(src, p);
        push(&t->src, &t->nsrc, full ? full : p);
        free(full);
    }
}

static void add_compile(MIG_TARGET *t, const JV *j) {
    JV *gs = jv_get(j, "compileGroups");
    if (!gs || gs->type != JARR) return;

    for (int g = 0; g < gs->n; g++) {
        const JV *grp = gs->items[g];
        JV       *inc = jv_get(grp, "includes");
        JV       *def = jv_get(grp, "defines");
        JV       *frs = jv_get(grp, "compileCommandFragments");
        if (inc && inc->type == JARR)
            for (int i = 0; i < inc->n; i++) {
                const char *p = jv_str(jv_get(inc->items[i], "path"));
                if (p) push_uniq(&t->inc, &t->ninc, p);
            }

        if (def && def->type == JARR)
            for (int i = 0; i < def->n; i++) {
                const char *d = jv_str(jv_get(def->items[i], "define"));
                if (!d) continue;
                char buf[2048];
                snprintf(buf, sizeof(buf), "-D%s", d);
                push_uniq(&t->cflags, &t->ncflags, buf);
            }

        if (frs && frs->type == JARR)
            for (int i = 0; i < frs->n; i++) {
                const char *f = jv_str(jv_get(frs->items[i], "fragment"));
                if (f && *f) push_uniq(&t->cflags, &t->ncflags, f);
            }
    }
}

static void add_link(MIG_TARGET *t, const JV *j) {
    JV *link = jv_get(j, "link");
    JV *frs  = link ? jv_get(link, "commandFragments") : NULL;
    if (!frs || frs->type != JARR) return;

    for (int i = 0; i < frs->n; i++) {
        const char *fr   = jv_str(jv_get(frs->items[i], "fragment"));
        const char *role = jv_str(jv_get(frs->items[i], "role"));
        if (!fr || !*fr) continue;

        if ((!role || strcmp(role, "libraries")) || (!strstr(fr, ".a") && !strstr(fr, ".lib")))
            push_uniq(&t->ldflags, &t->nldflags, fr);
    }
}

static void add_deps(MIG_SET *m, MIG_TARGET *t, const JV *j) {
    const char *keys[] = {"linkLibraries", "dependencies", NULL};
    for (int k = 0; keys[k]; k++) {
        JV *a = jv_get(j, keys[k]);
        if (!a || a->type != JARR) continue;

        for (int i = 0; i < a->n; i++) {
            const char *id  = jv_str(jv_get(a->items[i], "id"));
            const char *frg = jv_str(jv_get(a->items[i], "fragment"));
            if (id && *id) {
                char name[512];
                id_to_name(id, name, sizeof(name));
                if (name[0] && target_get(m, name)) push_uniq(&t->deps, &t->ndeps, name);
            } else if (frg && *frg) {
                char buf[2048];
                if (frg[0] == '-') push_uniq(&t->ldflags, &t->nldflags, frg);
                else {
                    snprintf(buf, sizeof(buf), "-l%s", frg);
                    push_uniq(&t->ldflags, &t->nldflags, buf);
                }
            }
        }
    }
}

static void grab_ldscript(MIG_TARGET *t) {
    for (int i = 0; i < t->nldflags;) {
        const char *f   = t->ldflags[i];
        const char *val = NULL;
        if (!strcmp(f, "-T") && i + 1 < t->nldflags) val = t->ldflags[i + 1];
        else if (!strncmp(f, "-T", 2) && f[2]) val = f + 2;

        if (!val) {
            i++;
            continue;
        }

        free(t->ldscript);
        t->ldscript = sys_dup(val);
        int drop    = (val == f + 2) ? 1 : 2;
        for (int k = 0; k < drop; k++) free(t->ldflags[i + k]);
        memmove(&t->ldflags[i], &t->ldflags[i + drop],
                sizeof(char *) * (size_t)(t->nldflags - i - drop));
        t->nldflags -= drop;
    }
}

int cmake_api_load(MIG_SET *s, const char *srcdir, const char *builddir_hint,
                   const char *config_hint, char **args, int nargs, char *err, size_t errsz) {
    char builddir[4096];
    if (builddir_hint && *builddir_hint) snprintf(builddir, sizeof(builddir), "%s", builddir_hint);
    else snprintf(builddir, sizeof(builddir), "%s/build-heddle-cmake", srcdir);

    char qdir[8192];
    snprintf(qdir, sizeof(qdir), "%s/.cmake/api/v1/query", builddir);
    mkdirs(qdir);
    const char *qk[] = {"codemodel-v2", "cache-v2", "toolchains-v1", "cmakeFiles-v1"};
    for (int i = 0; i < 4; i++) write_empty(qdir, qk[i]);

    char log[8192];
    snprintf(log, sizeof(log), "%s/heddle-cmake-configure.log", builddir);
    char *argv[64] = {"cmake", "-S", (char *)srcdir, "-B", builddir};
    int   n        = 5;
    for (int i = 0; i < nargs && n < 63; i++) argv[n++] = args[i];
    argv[n] = NULL;
    PROC_RESULT r;
    if (proc_run(argv, NULL, log, NULL, 0, &r) != 0) {
        snprintf(err, errsz, "failed to run cmake (is it on PATH?)");
        return -1;
    }
    if (r.exit_code != 0) {
        snprintf(err, errsz, "cmake configure failed (see %s)", log);
        return -1;
    }

    char rdir[8192];
    snprintf(rdir, sizeof(rdir), "%s/.cmake/api/v1/reply", builddir);
    char *ip  = find_reply(rdir, "index-");
    JV   *idx = ip ? json_load(ip) : NULL;
    free(ip);
    if (!idx) {
        snprintf(err, errsz, "no CMake File API reply in %s (need CMake >= 3.14)", rdir);
        return -1;
    }

    JV         *cm  = jv_get(jv_get(idx, "reply"), "codemodel-v2");
    const char *cmf = cm ? jv_str(jv_get(cm, "jsonFile")) : NULL;
    if (!cmf) {
        jv_free(idx);
        snprintf(err, errsz, "no codemodel-v2 reply");
        return -1;
    }

    char *cmp   = join_from(rdir, cmf);
    JV   *model = cmp ? json_load(cmp) : NULL;
    free(cmp);
    jv_free(idx);
    if (!model) {
        snprintf(err, errsz, "cannot parse codemodel-v2");
        return -1;
    }

    JV *cfgs = jv_get(model, "configurations");
    if (!cfgs || cfgs->type != JARR || cfgs->n == 0) {
        jv_free(model);
        snprintf(err, errsz, "codemodel has no configurations");
        return -1;
    }

    int ci = 0;
    if (config_hint && *config_hint) {
        for (ci = 0; ci < cfgs->n; ci++) {
            const char *nm = jv_str(jv_get(cfgs->items[ci], "name"));
            if (nm && !strcmp(nm, config_hint)) break;
        }
        if (ci == cfgs->n) {
            jv_free(model);
            snprintf(err, errsz, "no configuration '%s'", config_hint);
            return -1;
        }
    }

    JV    *trefs = jv_get(cfgs->items[ci], "targets");
    JV   **tjs   = NULL;
    char **tns   = NULL;
    int    ntg   = 0;
    for (int i = 0; trefs && trefs->type == JARR && i < trefs->n; i++) {
        const char *nm = jv_str(jv_get(trefs->items[i], "name"));
        const char *jf = jv_str(jv_get(trefs->items[i], "jsonFile"));
        if (!nm || !jf) continue;

        char *tp = join_from(rdir, jf);
        JV   *tg = tp ? json_load(tp) : NULL;
        free(tp);
        if (!tg) continue;

        const char *ty = jv_str(jv_get(tg, "type"));
        if ((ty && !strcmp(ty, "INTERFACE_LIBRARY")) || is_internal(nm, ty)) {
            jv_free(tg);
            continue;
        }

        MIG_TARGET *t = target_add(s, nm);
        free(t->type);
        t->type = sys_dup(map_type(ty));
        add_sources(t, tg, srcdir);
        add_compile(t, tg);
        add_link(t, tg);
        tjs      = realloc(tjs, sizeof(JV *) * (size_t)(ntg + 1));
        tns      = realloc(tns, sizeof(char *) * (size_t)(ntg + 1));
        tjs[ntg] = tg;
        tns[ntg] = sys_dup(nm);
        ntg++;
    }

    for (int i = 0; i < ntg; i++) add_deps(s, target_get(s, tns[i]), tjs[i]);
    for (int i = 0; i < ntg; i++) {
        jv_free(tjs[i]);
        free(tns[i]);
    }
    free(tjs);
    free(tns);
    jv_free(model);
    if (s->n == 0) {
        snprintf(err, errsz, "no importable targets");
        return -1;
    }

    for (int i = 0; i < s->n; i++) grab_ldscript(&s->items[i]);

    for (int i = 0; i < s->n; i++)
        for (int k = 0; k < s->items[i].nsrc; k++)
            for (int j = i + 1; j < s->n; j++)
                for (int m = 0; m < s->items[j].nsrc; m++)
                    if (!strcmp(s->items[i].src[k], s->items[j].src[m])) {
                        char msg[1200];
                        snprintf(msg, sizeof(msg), "source '%s' used by both '%s' and '%s'",
                                 s->items[i].src[k], s->items[i].name, s->items[j].name);
                        warn(s, msg);
                    }

    if (s->nwarn == 0) warn(s, "imported via CMake File API");
    return 0;
}
