#ifndef EMIT_H
#define EMIT_H

#include <stddef.h>

#include "project.h"

void emit_set_self(const char *argv0);

int emit_graph(const PROJECT *p, const char *target, const char *graph,
               char *err, size_t errsz);

int deps_ready(const PROJECT *p, char *err, size_t errsz);

char *emit_artifact(const PROJECT *p, const TARGET *t);

int emit_compile_db(const PROJECT *p, const char *target, const char *out,
                    char *err, size_t errsz);

#endif
