#include "migrate.h"

#include "sys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void push(char ***v, int *n, const char *s) {
    char **next = (char **)realloc(*v, sizeof(char *) * (size_t)(*n + 1));
    if (!next) return;

    *v = next;
    (*v)[*n] = sys_dup(s);

    if ((*v)[*n]) (*n)++;
}

static MIG_TARGET *target_add(MIG_SET *s, const char *name) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->items[i].name, name)) return &s->items[i];

    if (s->n == s->cap) {
        int newcap = s->cap ? s->cap * 2 : 8;

        MIG_TARGET *items = (MIG_TARGET *)realloc(
            s->items, sizeof(MIG_TARGET) * (size_t)newcap);
        if (!items) return NULL;

        s->items = items;
        s->cap   = newcap;
    }

    MIG_TARGET *t = &s->items[s->n++];
    memset(t, 0, sizeof(*t));

    t->name = sys_dup(name);
    return t;
}

static MIG_TARGET *target_get(MIG_SET *s, const char *name) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->items[i].name, name)) return &s->items[i];

    return NULL;
}

static void warn(MIG_SET *s, const char *fmt, const char *a) {
    char buf[1024];

    snprintf(buf, sizeof(buf), fmt, a);
    push(&s->warn, &s->nwarn, buf);
}

void migrate_free(MIG_SET *s) {
    for (int i = 0; i < s->n; i++) {
        MIG_TARGET *t = &s->items[i];

        free(t->name);
        free(t->type);
        free(t->ldscript);

        for (int k = 0; k < t->nsrc; k++)    free(t->src[k]);
        for (int k = 0; k < t->ninc; k++)    free(t->inc[k]);
        for (int k = 0; k < t->ndeps; k++)   free(t->deps[k]);
        for (int k = 0; k < t->ncflags; k++) free(t->cflags[k]);
        for (int k = 0; k < t->nldflags; k++) free(t->ldflags[k]);

        free(t->src);
        free(t->inc);
        free(t->deps);
        free(t->cflags);
        free(t->ldflags);
    }

    for (int i = 0; i < s->nwarn; i++) free(s->warn[i]);

    free(s->warn);
    free(s->items);
    memset(s, 0, sizeof(*s));
}

static char *slurp(const char *path, char *err, size_t errsz) {
    FILE *f = fopen(path, "rb");

    if (!f) {
        snprintf(err, errsz, "cannot read %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)n + 1);

    if (!buf) {
        fclose(f);
        snprintf(err, errsz, "out of memory");
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static void strip_comments(char *s) {
    int q = 0;
    int line = 0;

    for (char *p = s; *p; p++) {
        if (*p == '\n') { line = 0; continue; }
        if (q) { if (*p == '"') q = 0; continue; }
        if (*p == '"') { q = 1; continue; }

        if (!line && *p == '#') {
            char *e = p;

            while (*e && *e != '\n') *e++ = ' ';

            line = 1;
        }
    }
}

typedef struct {
    char **args;
    int    n;
} CMAKE_ARGS;

static void args_free(CMAKE_ARGS *a) {
    for (int i = 0; i < a->n; i++) free(a->args[i]);

    free(a->args);
    memset(a, 0, sizeof(*a));
}

static void split_args_ex(const char *s, const char *end, CMAKE_ARGS *out,
                         int commas) {
    const char *p = s;

    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) ||
                           (commas && *p == ','))) p++;

        if (p >= end) break;

        size_t cap = 64, n = 0;
        char  *buf = (char *)malloc(cap);

        if (!buf) return;

        int q = 0;

        while (p < end && (q || (commas
                                 ? (*p != ',' && !isspace((unsigned char)*p))
                                 : !isspace((unsigned char)*p)))) {
            if (*p == '"') { q = !q; p++; continue; }

            if (n + 2 >= cap) {
                cap *= 2;
                char *big = (char *)realloc(buf, cap);
                if (!big) { free(buf); return; }
                buf = big;
            }

            buf[n++] = *p++;
        }

        buf[n] = 0;

        if (n) push(&out->args, &out->n, buf);

        free(buf);
    }
}

static void split_args(const char *s, const char *end, CMAKE_ARGS *out) {
    split_args_ex(s, end, out, 0);
}

static const char *kind_of_cmake(const char *kw) {
    if (!kw) return "exe";
    if (!strcasecmp(kw, "STATIC")) return "staticlib";
    if (!strcasecmp(kw, "SHARED")) return "sharedlib";
    if (!strcasecmp(kw, "MODULE")) return "sharedlib";

    return "exe";
}

static void mig_resolve(MIG_SET *s) {
    for (int i = 0; i < s->n; i++) {
        MIG_TARGET *t = &s->items[i];

        for (int k = 0; k < t->ndeps; ) {
            const char *d = t->deps[k];

            if (target_get(s, d)) { k++; continue; }

            char l[1024];
            snprintf(l, sizeof(l), "-l%s", d);

            push(&t->ldflags, &t->nldflags, l);

            free(t->deps[k]);
            memmove(&t->deps[k], &t->deps[k + 1],
                    sizeof(char *) * (size_t)(t->ndeps - k - 1));
            t->ndeps--;
        }
    }
}

static int is_scope(const char *s) {
    return !strcasecmp(s, "PUBLIC") || !strcasecmp(s, "PRIVATE") ||
           !strcasecmp(s, "INTERFACE") || !strcasecmp(s, "SYSTEM");
}

int migrate_load_cmake(MIG_SET *s, const char *path, char *err, size_t errsz) {
    char *text = slurp(path, err, errsz);

    if (!text) return -1;

    strip_comments(text);

    const char *p = text;

    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ')')) p++;

        if (!*p) break;

        const char *name_b = p;

        while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;

        if (p == name_b) { p++; continue; }

        char name[128];
        size_t nl = (size_t)(p - name_b);

        if (nl >= sizeof(name)) { p++; continue; }

        memcpy(name, name_b, nl);
        name[nl] = 0;

        while (*p && isspace((unsigned char)*p)) p++;

        if (*p != '(') continue;

        p++;

        const char *ab = p;
        int depth = 1;

        while (*p && depth) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            if (depth) p++;
        }

        CMAKE_ARGS a;
        memset(&a, 0, sizeof(a));
        split_args(ab, p, &a);

        if (*p == ')') p++;

        if (!strcasecmp(name, "add_executable") && a.n >= 1) {
            MIG_TARGET *t = target_add(s, a.args[0]);

            if (t) {
                free(t->type);
                t->type = sys_dup("exe");

                for (int i = 1; i < a.n; i++) push(&t->src, &t->nsrc, a.args[i]);
            }
        } else if (!strcasecmp(name, "add_library") && a.n >= 2) {
            MIG_TARGET *t = target_add(s, a.args[0]);

            if (t) {
                free(t->type);
                t->type = sys_dup(kind_of_cmake(a.args[1]));

                for (int i = 2; i < a.n; i++) push(&t->src, &t->nsrc, a.args[i]);
            }
        } else if (!strcasecmp(name, "target_sources") && a.n >= 2) {
            MIG_TARGET *t = target_get(s, a.args[0]);

            for (int i = 1; i < a.n && t; i++)
                if (!is_scope(a.args[i])) push(&t->src, &t->nsrc, a.args[i]);
        } else if (!strcasecmp(name, "target_include_directories") && a.n >= 2) {
            MIG_TARGET *t = target_get(s, a.args[0]);

            for (int i = 1; i < a.n && t; i++)
                if (!is_scope(a.args[i])) push(&t->inc, &t->ninc, a.args[i]);
        } else if (!strcasecmp(name, "target_link_libraries") && a.n >= 2) {
            MIG_TARGET *t = target_get(s, a.args[0]);

            for (int i = 1; i < a.n && t; i++) {
                if (is_scope(a.args[i])) continue;

                if (a.args[i][0] == '-' || strchr(a.args[i], ':'))
                    push(&t->ldflags, &t->nldflags, a.args[i]);
                else
                    push(&t->deps, &t->ndeps, a.args[i]);
            }
        } else if (!strcasecmp(name, "target_compile_definitions") && a.n >= 2) {
            MIG_TARGET *t = target_get(s, a.args[0]);

            for (int i = 1; i < a.n && t; i++) {
                if (is_scope(a.args[i])) continue;

                char d[1024];
                snprintf(d, sizeof(d), "-D%s", a.args[i]);
                push(&t->cflags, &t->ncflags, d);
            }
        } else if (!strcasecmp(name, "target_compile_options") && a.n >= 2) {
            MIG_TARGET *t = target_get(s, a.args[0]);

            for (int i = 1; i < a.n && t; i++)
                if (!is_scope(a.args[i])) push(&t->cflags, &t->ncflags, a.args[i]);
        } else if (!strcasecmp(name, "target_link_options") && a.n >= 2) {
            MIG_TARGET *t = target_get(s, a.args[0]);

            for (int i = 1; i < a.n && t; i++) {
                if (is_scope(a.args[i])) continue;

                if (!strcmp(a.args[i], "-T") && i + 1 < a.n) {
                    free(t->ldscript);
                    t->ldscript = sys_dup(a.args[++i]);
                    continue;
                }

                if (!strncmp(a.args[i], "-T", 2) && a.args[i][2]) {
                    free(t->ldscript);
                    t->ldscript = sys_dup(a.args[i] + 2);
                    continue;
                }

                size_t vl = strlen(a.args[i]);

                if (vl > 4 && !strncmp(a.args[i], "-Wl,", 4) &&
                    !strncmp(a.args[i] + 4, "-T,", 3)) {
                    free(t->ldscript);
                    t->ldscript = sys_dup(a.args[i] + 7);
                    continue;
                }

                push(&t->ldflags, &t->nldflags, a.args[i]);
            }
        } else if (!strcasecmp(name, "add_definitions")) {
            for (int i = 0; i < s->n; i++) {
                for (int k = 0; k < a.n; k++) {
                    char d[1024];
                    snprintf(d, sizeof(d), "%s", a.args[k]);
                    push(&s->items[i].cflags, &s->items[i].ncflags, d);
                }
            }
        } else if (!strcasecmp(name, "set") && a.n >= 3 &&
                   !strcasecmp(a.args[1], "LINKER_LANGUAGE")) {
            warn(s, "ignored: set(%s)", a.args[0]);
        } else if ((!strcasecmp(name, "include_directories") ||
                    !strcasecmp(name, "add_compile_options"))) {
            warn(s, "global '%s' not translated, add it to targets by hand", name);
        }

        args_free(&a);
    }

    free(text);

    if (s->n == 0) {
        snprintf(err, errsz, "%s: no add_executable/add_library found", path);
        return -1;
    }

    mig_resolve(s);
    return 0;
}

static const char *xmake_kind(const char *k) {
    if (!k) return "exe";
    if (!strcasecmp(k, "static")) return "staticlib";
    if (!strcasecmp(k, "shared")) return "sharedlib";
    if (!strcasecmp(k, "binary")) return "exe";

    return "exe";
}

static void lua_strip(char *s) {
    for (char *p = s; *p; p++) {
        if (p[0] == '-' && p[1] == '-') {
            char *e = p;

            while (*e && *e != '\n') *e++ = ' ';

            break;
        }
    }
}

static CMAKE_ARGS lua_args(const char *line) {
    CMAKE_ARGS a;
    memset(&a, 0, sizeof(a));

    const char *lp = strchr(line, '(');
    const char *rp = lp ? strrchr(lp, ')') : NULL;

    if (!lp || !rp) return a;

    split_args_ex(lp + 1, rp, &a, 1);
    return a;
}

static const char *lua_fn(const char *line, char *buf, size_t cap) {
    while (*line && isspace((unsigned char)*line)) line++;

    const char *b = line;

    while (*line && (isalnum((unsigned char)*line) || *line == '_')) line++;

    size_t n = (size_t)(line - b);

    if (!n || n >= cap) return NULL;

    while (*line && isspace((unsigned char)*line)) line++;

    if (*line != '(') return NULL;

    memcpy(buf, b, n);
    buf[n] = 0;
    return buf;
}

int migrate_load_xmake(MIG_SET *s, const char *path, char *err, size_t errsz) {
    FILE *f = fopen(path, "r");

    if (!f) {
        snprintf(err, errsz, "cannot read %s", path);
        return -1;
    }

    char  line[8192];
    MIG_TARGET *cur = NULL;

    while (fgets(line, sizeof(line), f)) {
        lua_strip(line);

        char fn[64];

        if (!lua_fn(line, fn, sizeof(fn))) continue;

        CMAKE_ARGS a = lua_args(line);

        if (!strcasecmp(fn, "target") && a.n >= 1) {
            cur = target_add(s, a.args[0]);
        } else if (!cur) {
            args_free(&a);
            continue;
        } else if (!strcasecmp(fn, "set_kind") && a.n >= 1) {
            free(cur->type);
            cur->type = sys_dup(xmake_kind(a.args[0]));
        } else if (!strcasecmp(fn, "add_files") ||
                   !strcasecmp(fn, "add_headerfiles")) {
            for (int i = 0; i < a.n; i++) push(&cur->src, &cur->nsrc, a.args[i]);
        } else if (!strcasecmp(fn, "remove_files")) {
            for (int i = 0; i < a.n; i++) {
                int hit = 0;

                for (int k = 0; k < cur->nsrc; k++) {
                    if (strcmp(cur->src[k], a.args[i])) continue;

                    free(cur->src[k]);
                    memmove(&cur->src[k], &cur->src[k + 1],
                            sizeof(char *) * (size_t)(cur->nsrc - k - 1));
                    cur->nsrc--;
                    hit = 1;
                    break;
                }

                if (!hit) warn(s, "remove_files(%s) not matched, prune by hand",
                               a.args[i]);
            }
        } else if (!strcasecmp(fn, "add_includedirs")) {
            for (int i = 0; i < a.n; i++) push(&cur->inc, &cur->ninc, a.args[i]);
        } else if (!strcasecmp(fn, "add_deps") ||
                   !strcasecmp(fn, "add_links")) {
            for (int i = 0; i < a.n; i++) push(&cur->deps, &cur->ndeps, a.args[i]);
        } else if (!strcasecmp(fn, "add_defines")) {
            for (int i = 0; i < a.n; i++) {
                char d[1024];
                snprintf(d, sizeof(d), "-D%s", a.args[i]);
                push(&cur->cflags, &cur->ncflags, d);
            }
        } else if (!strcasecmp(fn, "add_cxflags") ||
                   !strcasecmp(fn, "add_cflags") ||
                   !strcasecmp(fn, "add_cxxflags")) {
            for (int i = 0; i < a.n; i++) push(&cur->cflags, &cur->ncflags, a.args[i]);
        } else if (!strcasecmp(fn, "add_ldflags") ||
                   !strcasecmp(fn, "add_ldscripts")) {
            for (int i = 0; i < a.n; i++) {
                if (!strcmp(a.args[i], "-T") && i + 1 < a.n) {
                    free(cur->ldscript);
                    cur->ldscript = sys_dup(a.args[++i]);
                    continue;
                }

                if (!strncmp(a.args[i], "-T", 2) && a.args[i][2]) {
                    free(cur->ldscript);
                    cur->ldscript = sys_dup(a.args[i] + 2);
                    continue;
                }

                push(&cur->ldflags, &cur->nldflags, a.args[i]);
            }
        } else if (!strcasecmp(fn, "add_syslinks")) {
            for (int i = 0; i < a.n; i++) {
                char l[1024];
                snprintf(l, sizeof(l), "-l%s", a.args[i]);
                push(&cur->ldflags, &cur->nldflags, l);
            }
        }

        args_free(&a);
    }

    fclose(f);

    if (s->n == 0) {
        snprintf(err, errsz, "%s: no target() found", path);
        return -1;
    }

    for (int i = 0; i < s->n; i++)
        if (!s->items[i].type) s->items[i].type = sys_dup("exe");

    mig_resolve(s);
    return 0;
}

static void emit_list(FILE *f, const char *key, char **v, int n) {
    if (!n) return;

    fprintf(f, "%s = [", key);

    for (int i = 0; i < n; i++)
        fprintf(f, "%s\"%s\"", i ? ", " : "", v[i]);

    fputs("]\n", f);
}

int migrate_write(const MIG_SET *s, const char *out, char *err, size_t errsz) {
    FILE *f = fopen(out, "w");

    if (!f) {
        snprintf(err, errsz, "cannot write %s", out);
        return -1;
    }

    fputs("[build]\ndir = \"out\"\n", f);

    for (int i = 0; i < s->n; i++) {
        const MIG_TARGET *t = &s->items[i];

        fprintf(f, "\n[target.%s]\ntype = \"%s\"\n", t->name,
                t->type ? t->type : "exe");

        emit_list(f, "src", t->src, t->nsrc);
        emit_list(f, "inc", t->inc, t->ninc);

        if (t->ndeps) emit_list(f, "deps", t->deps, t->ndeps);

        emit_list(f, "cflags", t->cflags, t->ncflags);
        emit_list(f, "ldflags", t->ldflags, t->nldflags);

        if (t->ldscript) fprintf(f, "linker_script = \"%s\"\n", t->ldscript);
    }

    fclose(f);
    return 0;
}

void migrate_print(const MIG_SET *s) {
    printf("heddle: %d target%s\n", s->n, s->n == 1 ? "" : "s");

    for (int i = 0; i < s->n; i++) {
        const MIG_TARGET *t = &s->items[i];

        printf("  %-16s %-10s src=%d inc=%d deps=%d\n", t->name,
               t->type ? t->type : "exe", t->nsrc, t->ninc, t->ndeps);
    }

    if (s->nwarn) {
        printf("heddle: %d note%s\n", s->nwarn, s->nwarn == 1 ? "" : "s");

        for (int i = 0; i < s->nwarn; i++)
            printf("  - %s\n", s->warn[i]);
    }
}

static const char *arg_opt(int argc, char **argv, const char *name) {
    size_t n = strlen(name);

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], name) && i + 1 < argc) return argv[i + 1];
        if (!strncmp(argv[i], name, n) && argv[i][n] == '=') return argv[i] + n + 1;
    }

    return NULL;
}

static void migrate_usage(void) {
    printf("usage: heddle migrate [DIR] [options]\n"
           "\n"
           "  reads CMakeLists.txt or xmake.lua and writes heddle.toml\n"
           "\n"
           "options:\n"
           "  --from cmake|xmake   force the source format\n"
           "  --out FILE           output file (default heddle.toml)\n"
           "  --dry-run            print the plan, write nothing\n");
}

int heddle_migrate(int argc, char **argv) {
    const char *dir     = NULL;
    const char *from    = arg_opt(argc, argv, "--from");
    const char *out     = arg_opt(argc, argv, "--out");
    int         dry     = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            migrate_usage();
            return 0;
        }

        if (!strcmp(argv[i], "--dry-run")) { dry = 1; continue; }

        if (!strncmp(argv[i], "--", 2)) {
            if (!strchr(argv[i], '=')) i++;
            continue;
        }

        if (argv[i][0] == '-') continue;

        if (!dir) dir = argv[i];
    }

    if (!dir) dir = ".";

    char cmake[4096];
    char xmake[4096];

    snprintf(cmake, sizeof(cmake), "%s/CMakeLists.txt", dir);
    snprintf(xmake, sizeof(xmake), "%s/xmake.lua", dir);

    const char *src  = NULL;
    int         kind = 0;

    if (from) {
        if (!strcmp(from, "cmake")) { src = cmake; kind = 1; }
        else if (!strcmp(from, "xmake")) { src = xmake; kind = 2; }
        else {
            fprintf(stderr, "heddle: unknown --from '%s'\n", from);
            return 2;
        }
    } else {
        SYS_STAT st;

        if (sys_stat(cmake, &st) == 0) { src = cmake; kind = 1; }
        else if (sys_stat(xmake, &st) == 0) { src = xmake; kind = 2; }
    }

    if (!src) {
        fprintf(stderr, "heddle: no %s/CMakeLists.txt or %s/xmake.lua\n",
                dir, dir);
        return 1;
    }

    SYS_STAT st;

    if (sys_stat(src, &st) != 0) {
        fprintf(stderr, "heddle: cannot read %s\n", src);
        return 1;
    }

    MIG_SET s;
    memset(&s, 0, sizeof(s));

    char err[256] = {0};

    int rc = kind == 1 ? migrate_load_cmake(&s, src, err, sizeof(err))
                       : migrate_load_xmake(&s, src, err, sizeof(err));

    if (rc != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        migrate_free(&s);
        return 1;
    }

    printf("heddle: from %s\n", src);
    migrate_print(&s);

    if (dry) {
        migrate_free(&s);
        return 0;
    }

    char def_out[4096];
    snprintf(def_out, sizeof(def_out), "%s/heddle.toml", dir);

    const char *target_out = out ? out : def_out;

    if (sys_stat(target_out, &st) == 0) {
        fprintf(stderr, "heddle: %s exists, use --out to pick another name\n",
                target_out);
        migrate_free(&s);
        return 1;
    }

    if (migrate_write(&s, target_out, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        migrate_free(&s);
        return 1;
    }

    printf("heddle: wrote %s\n", target_out);

    migrate_free(&s);
    return 0;
}
