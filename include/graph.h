#ifndef GRAPH_H
#define GRAPH_H

#include <stddef.h>

typedef struct {
    char **argv;
    int    argc;

    int   *deps;
    int    ndeps;
    int   *rdeps;
    int    nrdeps;
    int    indeg;

    char **ins;
    int    nins;
    char **outs;
    int    nouts;

    unsigned long long cmd_hash;
} NODE;

typedef struct {
    NODE *nodes;
    int   n;
} GRAPH;

int graph_parse(const char *text, GRAPH *g, char *err, size_t errsz);

void graph_free(GRAPH *g);

#endif
