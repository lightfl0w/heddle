#define _POSIX_C_SOURCE 200809L

#include "deps.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char **items;
    int    count;
    int    cap;
} STRSET;

static int set_has(const STRSET *s, const char *v) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->items[i], v)) return 1;

    return 0;
}

static int set_add(STRSET *s, const char *v) {
    if (set_has(s, v)) return 0;

    if (s->count == s->cap) {
        int newcap = s->cap ? s->cap * 2 : 16;

        char **items = (char **)realloc(s->items, sizeof(char *) * (size_t)newcap);
        if (!items) return -1;

        s->items = items;
        s->cap   = newcap;
    }

    s->items[s->count] = strdup(v);
    if (!s->items[s->count]) return -1;

    s->count++;
    return 0;
}

static void set_free(STRSET *s) {
    for (int i = 0; i < s->count; i++)
        free(s->items[i]);

    free(s->items);
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');

    if (!slash) return strdup(".");
    if (slash == path) return strdup("/");

    size_t n = (size_t)(slash - path);
    char  *d = (char *)malloc(n + 1);
    if (!d) return NULL;

    memcpy(d, path, n);
    d[n] = 0;
    return d;
}

static char *join_path(const char *dir, const char *name) {
    if (!strcmp(dir, ".")) return strdup(name);

    size_t n = strlen(dir) + strlen(name) + 2;
    char  *p = (char *)malloc(n);
    if (!p) return NULL;

    snprintf(p, n, "%s/%s", dir, name);
    return p;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;

    return s;
}

static int resolve_include(const NODE *nd, const char *from_dir,
                           const char *name, int angled, char **out) {
    if (!angled) {
        char *cand = join_path(from_dir, name);
        if (cand && file_exists(cand)) {
            *out = cand;
            return 1;
        }

        free(cand);
    }

    for (int i = 0; i < nd->nincdirs; i++) {
        char *cand = join_path(nd->incdirs[i], name);
        if (cand && file_exists(cand)) {
            *out = cand;
            return 1;
        }

        free(cand);
    }

    char *cand = join_path(".", name);
    if (cand && file_exists(cand)) {
        *out = cand;
        return 1;
    }

    free(cand);
    return 0;
}

static int scan_file(GRAPH *g, int node, const char *path, STRSET *visited) {
    if (set_has(visited, path)) return 0;
    if (set_add(visited, path) != 0) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char *dir = dir_of(path);
    char  line[4096];

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);

        if (strncmp(s, "#", 1) != 0) continue;

        s = trim(s + 1);
        if (strncmp(s, "include", 7) != 0) continue;

        s = trim(s + 7);
        if (*s != '"' && *s != '<') continue;

        char close = (*s == '"') ? '"' : '>';
        char *end  = strchr(s + 1, close);
        if (!end) continue;

        *end = 0;

        char *name = s + 1;
        char *resolved = NULL;

        if (!resolve_include(&g->nodes[node], dir ? dir : ".",
                             name, *s == '<', &resolved))
            continue;

        graph_node_add_dyn(&g->nodes[node], resolved);
        scan_file(g, node, resolved, visited);

        free(resolved);
    }

    free(dir);
    fclose(f);
    return 0;
}

static unsigned long long input_key(const NODE *nd, HASH_DB *files) {
    unsigned long long k = nd->cmd_hash;

    for (int i = 0; i < nd->nins; i++)
        k = hash_u64(k, hash_read_file(files, nd->ins[i]));

    return k;
}

static int scan_node(GRAPH *g, int node) {
    NODE *nd = &g->nodes[node];

    graph_node_clear_dyn(nd);

    STRSET visited = {0};

    for (int i = 0; i < nd->nins; i++)
        scan_file(g, node, nd->ins[i], &visited);

    set_free(&visited);
    return 0;
}

int deps_scan(GRAPH *g, const char *cwd, HASH_DB *files) {
    (void)cwd;

    for (int i = 0; i < g->n; i++) {
        NODE *nd = &g->nodes[i];

        unsigned long long key = input_key(nd, files);
        if (key == nd->scan_hash) continue;

        scan_node(g, i);
        nd->scan_hash = key;
    }

    return 0;
}

int deps_load(GRAPH *g, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[4096];

    while (fgets(line, sizeof(line), f)) {
        int node = -1;
        unsigned long long key = 0;
        char rest[4096] = {0};

        if (sscanf(line, "D %d %llx %4095[^\n]", &node, &key, rest) < 2) continue;
        if (node < 0 || node >= g->n) continue;

        graph_node_clear_dyn(&g->nodes[node]);

        char *save = NULL;
        char *tok  = strtok_r(rest, " \t\r\n", &save);

        while (tok) {
            graph_node_add_dyn(&g->nodes[node], tok);
            tok = strtok_r(NULL, " \t\r\n", &save);
        }

        g->nodes[node].scan_hash = key;
    }

    fclose(f);
    return 0;
}

int deps_save(GRAPH *g, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    for (int i = 0; i < g->n; i++) {
        NODE *nd = &g->nodes[i];

        fprintf(f, "D %d %llx", i, nd->scan_hash);

        for (int k = 0; k < nd->ndyn; k++)
            fprintf(f, " %s", nd->dyn[k]);

        fputc('\n', f);
    }

    fclose(f);
    return 0;
}
