#ifndef DEPS_H
#define DEPS_H

#include "graph.h"
#include "hash.h"

int deps_load(GRAPH *g, const char *path);
int deps_save(GRAPH *g, const char *path);
int deps_scan(GRAPH *g, const char *cwd, HASH_DB *files);
#endif
