#ifndef GRAPH_H
#define GRAPH_H

#include <stddef.h>

typedef struct {
    char **argv;
    int    argc;

    int   *deps;
    int    ndeps;
    int    ndeps_static;
    int   *rdeps;
    int    nrdeps;
    int    indeg;

    char **ins;
    int    nins;
    char **outs;
    int    nouts;

    char **incdirs;
    int    nincdirs;

    char **dyn;
    int    ndyn;

    unsigned long long scan_hash;

    unsigned long long cmd_hash;
} NODE;

typedef struct {
    NODE *nodes;
    int   n;
} GRAPH;

int graph_parse(const char *text, GRAPH *g, char *err, size_t errsz);
void graph_free(GRAPH *g);
int graph_finalize(GRAPH *g, char *err, size_t errsz);
int graph_node_add_dyn(NODE *nd, const char *path);
int graph_node_has_dyn(const NODE *nd, const char *path);
void graph_node_clear_dyn(NODE *nd);

#endif

