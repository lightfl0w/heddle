#include "graph.h"

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

        const char *start = s;
        while (*s && !isspace((unsigned char)*s)) s++;

        size_t len = (size_t)(s - start);
        char  *tok = (char *)malloc(len + 1);
        if (!tok) goto fail;

        memcpy(tok, start, len);
        tok[len] = 0;

        if ((size_t)n == cap) {
            cap *= 2;

            char **nv = (char **)realloc(v, sizeof(char *) * (cap + 1));
            if (!nv) { free(tok); goto fail; }

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

static int build_rdeps(GRAPH *g, char *err, size_t errsz) {
    size_t n_alloc = g->n > 0 ? (size_t)g->n : 1;

    int *rcap = (int *)calloc(n_alloc, sizeof(int));
    if (!rcap) { set_err(err, errsz, "oom"); return -1; }

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
        if (!line) { set_err(err, errsz, "oom"); return -1; }

        memcpy(line, p, linelen);
        line[linelen] = 0;

        char *s   = line;
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
        char *lt   = strchr(rest, '<');
        char *deps_str = NULL;

        if (lt) { *lt = 0; deps_str = lt + 1; }

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

void graph_free(GRAPH *g) {
    if (!g->nodes) return;

    for (int i = 0; i < g->n; i++) {
        NODE *nd = &g->nodes[i];

        if (nd->argv) {
            for (int j = 0; j < nd->argc; j++) free(nd->argv[j]);
            free(nd->argv);
        }
        free(nd->deps);
        free(nd->rdeps);
    }

    free(g->nodes);
    g->nodes = NULL;
    g->n = 0;
}
