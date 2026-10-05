#ifndef EMIT_H
#define EMIT_H

#include <stddef.h>

#include "project.h"

int emit_graph(const PROJECT *p, const char *target, const char *graph,
               char *err, size_t errsz);

char *emit_artifact(const PROJECT *p, const TARGET *t);

#endif
