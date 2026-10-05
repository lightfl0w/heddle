#include "graph.h"
#include "hash.h"
#include "sys.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t errsz, const char *fmt, ...) {
    if (!err || errsz == 0) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static char **split_ws(const char *s, int *outn) {
    size_t cap = 8;
    int    n   = 0;
    char **v   = (char **)malloc(sizeof(char *) * (cap + 1));
    if (!v) return NULL;

    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;

        char   buf[8192];
        size_t len = 0;

        while (*s && !isspace((unsigned char)*s)) {
            char q = *s;

            if (q != '"' && q != '\'') {
                if (len + 1 < sizeof(buf)) buf[len++] = *s;
                s++;
                continue;
            }

            s++;

            while (*s && *s != q) {
                if (*s == '\\' && s[1]) s++;

                if (len + 1 < sizeof(buf)) buf[len++] = *s;

                s++;
            }

            if (*s == q) s++;
        }

        buf[len] = 0;

        char *tok = (char *)malloc(len + 1);
        if (!tok) goto fail;

        memcpy(tok, buf, len + 1);

        if ((size_t)n == cap) {
            cap *= 2;

            char **nv = (char **)realloc(v, sizeof(char *) * (cap + 1));
            if (!nv) {
                free(tok);
                goto fail;
            }

            v = nv;
        }

        v[n++] = tok;
    }

    v[n] = NULL;
    *outn = n;
    return v;

fail:
    for (int i = 0; i < n; i++) free(v[i]);

    free(v);
    return NULL;
}

static char *find_unquoted(char *s, char c) {
    char q = 0;

    for (; *s; s++) {
        if (q) {
            if (*s == q) q = 0;
            continue;
        }

        if (*s == '"' || *s == '\'') {
            q = *s;
            continue;
        }

        if (*s == c) return s;
    }

    return NULL;
}

static int graph_alloc(GRAPH *g, int cap) {
    g->nodes = (NODE *)calloc((size_t)cap, sizeof(NODE));
    return g->nodes ? 0 : -1;
}

static int graph_grow(GRAPH *g, int *cap) {
    *cap *= 2;

    NODE *nodes = (NODE *)realloc(g->nodes, sizeof(NODE) * (size_t)*cap);
    if (!nodes) return -1;

    g->nodes = nodes;
    return 0;
}

static int parse_deps(NODE *nd, const char *deps_str, int node_idx,
                      char *err, size_t errsz, int lineno) {
    int dn = 0;

    char **dtoks = split_ws(deps_str, &dn);
    if (!dtoks) return 0;

    size_t dcap = dn > 0 ? (size_t)dn : 1;
    nd->deps = (int *)malloc(sizeof(int) * dcap);

    if (!nd->deps) {
        for (int i = 0; i < dn; i++) free(dtoks[i]);

        free(dtoks);
        return -1;
    }

    nd->ndeps = 0;

    for (int i = 0; i < dn; i++) {
        int d = atoi(dtoks[i]);

        if (d < 0 || d >= node_idx) {
            set_err(err, errsz, "line %d: bad dependency %d", lineno, d);

            for (int k = 0; k < dn; k++) free(dtoks[k]);

            free(dtoks);
            return -1;
        }

        nd->deps[nd->ndeps++] = d;
    }

    for (int i = 0; i < dn; i++) free(dtoks[i]);

    free(dtoks);
    return 0;
}

static int list_add(char ***v, int *n, const char *s) {
    char **nv = (char **)realloc(*v, sizeof(char *) * (size_t)(*n + 1));
    if (!nv) return -1;

    nv[*n] = sys_dup(s);
    if (!nv[*n]) return -1;

    *v = nv;
    (*n)++;
    return 0;
}

static int looks_like_path(const char *s) {
    return s[0] != '-' && (strchr(s, '.') || strchr(s, '/'));
}

static int is_own_output(const NODE *nd, const char *s) {
    for (int i = 0; i < nd->nouts; i++)
        if (!strcmp(nd->outs[i], s)) return 1;

    return 0;
}

static void list_add_words(char ***v, int *n, char *words, int skip_outs,
                           const NODE *nd) {
    int    k = 0;
    char **tok = split_ws(words, &k);

    for (int i = 0; tok && i < k; i++) {
        if (!skip_outs || !is_own_output(nd, tok[i]))
            list_add(v, n, tok[i]);

        free(tok[i]);
    }

    free(tok);
}

static void node_scan(NODE *nd) {
    nd->cmd_hash = HASH_FNV_OFFSET;

    for (int i = 0; i < nd->argc; i++)
        nd->cmd_hash = hash_text(nd->cmd_hash, nd->argv[i]);

    for (int i = 1; i < nd->argc; i++)
        if (!strcmp(nd->argv[i], "-o") && i + 1 < nd->argc)
            list_add(&nd->outs, &nd->nouts, nd->argv[++i]);

    for (int i = 1; i < nd->argc; i++) {
        const char *arg = nd->argv[i];

        if (!strcmp(arg, "-I") && i + 1 < nd->argc) {
            list_add(&nd->incdirs, &nd->nincdirs, nd->argv[++i]);
            continue;
        }

        if (!strncmp(arg, "-I", 2) && arg[2] != 0) {
            list_add(&nd->incdirs, &nd->nincdirs, arg + 2);
            continue;
        }

        if (!strcmp(arg, "-o")) {
            i++;
            continue;
        }

        if (!looks_like_path(arg)) continue;
        if (is_own_output(nd, arg)) continue;

        list_add(&nd->ins, &nd->nins, arg);
    }
}

static void node_free(NODE *nd) {
    if (nd->argv) {
        for (int j = 0; j < nd->argc; j++) free(nd->argv[j]);

        free(nd->argv);
    }

    for (int j = 0; j < nd->nins; j++) free(nd->ins[j]);

    for (int j = 0; j < nd->nouts; j++) free(nd->outs[j]);

    for (int j = 0; j < nd->nincdirs; j++) free(nd->incdirs[j]);

    for (int j = 0; j < nd->ndyn; j++) free(nd->dyn[j]);

    free(nd->ins);
    free(nd->outs);
    free(nd->incdirs);
    free(nd->dyn);
    free(nd->deps);
    free(nd->rdeps);
}

static int build_rdeps(GRAPH *g, char *err, size_t errsz) {
    size_t n_alloc = g->n > 0 ? (size_t)g->n : 1;

    int *rcap = (int *)calloc(n_alloc, sizeof(int));
    if (!rcap) {
        set_err(err, errsz, "oom");
        return -1;
    }

    for (int i = 0; i < g->n; i++)
        for (int j = 0; j < g->nodes[i].ndeps; j++)
            rcap[g->nodes[i].deps[j]]++;

    for (int i = 0; i < g->n; i++) {
        size_t cap_i = rcap[i] > 0 ? (size_t)rcap[i] : 1;

        g->nodes[i].rdeps = (int *)malloc(sizeof(int) * cap_i);
        if (!g->nodes[i].rdeps) {
            free(rcap);
            set_err(err, errsz, "oom");
            return -1;
        }

        g->nodes[i].nrdeps = 0;
    }

    for (int i = 0; i < g->n; i++)
        for (int j = 0; j < g->nodes[i].ndeps; j++) {
            int d = g->nodes[i].deps[j];
            g->nodes[d].rdeps[g->nodes[d].nrdeps++] = i;
        }

    free(rcap);
    return 0;
}

static int detect_cycle(const GRAPH *g, char *err, size_t errsz) {
    size_t n = (size_t)g->n;
    if (n == 0) return 0;

    int *indeg = (int *)malloc(sizeof(int) * n);
    int *queue = (int *)malloc(sizeof(int) * n);

    if (!indeg || !queue) {
        free(indeg);
        free(queue);
        set_err(err, errsz, "oom");
        return -1;
    }

    for (size_t i = 0; i < n; i++) indeg[i] = g->nodes[i].indeg;

    int qh = 0;
    int qt = 0;

    for (size_t i = 0; i < n; i++)
        if (indeg[i] == 0) queue[qt++] = (int)i;

    int visited = 0;

    while (qh < qt) {
        int u = queue[qh++];
        visited++;

        for (int k = 0; k < g->nodes[u].nrdeps; k++) {
            int v = g->nodes[u].rdeps[k];
            if (--indeg[v] == 0) queue[qt++] = v;
        }
    }

    free(indeg);
    free(queue);

    if ((size_t)visited < n) {
        set_err(err, errsz, "cycle detected in DAG");
        return -1;
    }

    return 0;
}

static int node_has_dep(const NODE *nd, int d) {
    for (int i = 0; i < nd->ndeps; i++)
        if (nd->deps[i] == d) return 1;

    return 0;
}

static int node_add_dep(NODE *nd, int d) {
    if (node_has_dep(nd, d)) return 0;

    int *deps = (int *)realloc(nd->deps, sizeof(int) * (size_t)(nd->ndeps + 1));
    if (!deps) return -1;

    nd->deps = deps;
    nd->deps[nd->ndeps++] = d;
    return 0;
}

static int node_produces(const NODE *nd, const char *path) {
    for (int i = 0; i < nd->nouts; i++)
        if (!strcmp(nd->outs[i], path)) return 1;

    return 0;
}

static void free_rdeps(GRAPH *g) {
    for (int i = 0; i < g->n; i++) {
        free(g->nodes[i].rdeps);
        g->nodes[i].rdeps  = NULL;
        g->nodes[i].nrdeps = 0;
    }
}

int graph_finalize(GRAPH *g, char *err, size_t errsz) {
    for (int i = 0; i < g->n; i++) {
        NODE *nd = &g->nodes[i];

        nd->ndeps = nd->ndeps_static;

        for (int k = 0; k < nd->ndyn; k++)
            for (int j = 0; j < g->n; j++) {
                if (j == i) continue;
                if (!node_produces(&g->nodes[j], nd->dyn[k])) continue;

                if (node_add_dep(nd, j) != 0) {
                    set_err(err, errsz, "oom");
                    return -1;
                }
            }
    }

    free_rdeps(g);

    for (int i = 0; i < g->n; i++)
        g->nodes[i].indeg = g->nodes[i].ndeps;

    if (build_rdeps(g, err, errsz) != 0) return -1;

    return detect_cycle(g, err, errsz);
}

int graph_parse(const char *text, GRAPH *g, char *err, size_t errsz) {
    memset(g, 0, sizeof(*g));

    int cap = 16;
    if (graph_alloc(g, cap) != 0) {
        set_err(err, errsz, "oom");
        return -1;
    }

    int node_idx = 0;
    int lineno   = 0;
    const char *p = text;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen  = eol ? (size_t)(eol - p) : strlen(p);

        char *line = (char *)malloc(linelen + 1);
        if (!line) {
            set_err(err, errsz, "oom");
            return -1;
        }

        memcpy(line, p, linelen);
        line[linelen] = 0;

        char *s = line;
        while (*s && isspace((unsigned char)*s)) s++;

        char *end = s + strlen(s);
        while (end > s && isspace((unsigned char)end[-1])) *--end = 0;

        if (*s == 0 || *s == '#') {
            free(line);
            p = eol ? eol + 1 : p + linelen;
            lineno++;
            continue;
        }

        char *colon = strchr(s, ':');
        if (!colon) {
            set_err(err, errsz, "line %d: missing ':'", lineno + 1);
            free(line);
            return -1;
        }

        *colon = 0;

        int declared = atoi(s);
        if (declared != node_idx) {
            set_err(err, errsz, "line %d: expected index %d, got %d",
                    lineno + 1, node_idx, declared);
            free(line);
            return -1;
        }

        char *rest = colon + 1;
        char *lt   = find_unquoted(rest, '<');
        char *deps_str = NULL;

        if (lt) {
            *lt = 0;
            deps_str = lt + 1;
        }

        char *at = find_unquoted(rest, '@');
        char *ins_str = NULL;

        if (at) {
            *at = 0;
            ins_str = at + 1;
        }

        char *gt = find_unquoted(rest, '>');
        char *outs_str = NULL;

        if (gt) {
            *gt = 0;
            outs_str = gt + 1;
        }

        int    argc = 0;
        char **argv = split_ws(rest, &argc);

        if (!argv || argc == 0) {
            set_err(err, errsz, "line %d: empty command", lineno + 1);
            free(line);
            return -1;
        }

        if (g->n == cap && graph_grow(g, &cap) != 0) {
            set_err(err, errsz, "oom");
            free(line);
            return -1;
        }

        NODE *nd = &g->nodes[g->n];
        memset(nd, 0, sizeof(*nd));

        nd->argv  = argv;
        nd->argc  = argc;
        nd->indeg = 0;

        if (deps_str && parse_deps(nd, deps_str, node_idx,
                                   err, errsz, lineno + 1) != 0) {
            free(line);
            return -1;
        }

        nd->ndeps_static = nd->ndeps;

        node_scan(nd);

        if (ins_str)  list_add_words(&nd->ins, &nd->nins, ins_str, 0, nd);
        if (outs_str) list_add_words(&nd->outs, &nd->nouts, outs_str, 1, nd);

        g->n++;
        node_idx++;

        free(line);
        p = eol ? eol + 1 : p + linelen;
        lineno++;
    }

    for (int i = 0; i < g->n; i++)
        g->nodes[i].indeg = g->nodes[i].ndeps;

    if (build_rdeps(g, err, errsz) != 0) return -1;

    return detect_cycle(g, err, errsz);
}

int graph_node_add_dyn(NODE *nd, const char *path) {
    if (graph_node_has_dyn(nd, path)) return 0;

    return list_add(&nd->dyn, &nd->ndyn, path);
}

int graph_node_has_dyn(const NODE *nd, const char *path) {
    for (int i = 0; i < nd->ndyn; i++)
        if (!strcmp(nd->dyn[i], path)) return 1;

    return 0;
}

void graph_node_clear_dyn(NODE *nd) {
    for (int i = 0; i < nd->ndyn; i++) free(nd->dyn[i]);

    free(nd->dyn);
    nd->dyn = NULL;
    nd->ndyn = 0;
}

void graph_free(GRAPH *g) {
    if (!g->nodes) return;

    for (int i = 0; i < g->n; i++)
        node_free(&g->nodes[i]);

    free(g->nodes);
    g->nodes = NULL;
    g->n = 0;
}
