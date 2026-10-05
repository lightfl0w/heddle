#ifndef INCREMENTAL_H
#define INCREMENTAL_H

#include "graph.h"
#include "hash.h"

typedef struct {
    unsigned long long cmd_hash;
    unsigned long long in_hash;
    unsigned long long out_hash;
    int                valid;
} NODE_STATE;

typedef struct {
    const GRAPH *g;
    HASH_DB      files;
    NODE_STATE  *states;
    int          n;
} INCR_DB;

void incr_init(INCR_DB *db, const GRAPH *g);
void incr_free(INCR_DB *db);
int incr_load(INCR_DB *db, const char *path);
int incr_save(const INCR_DB *db, const char *path);
int incr_plan(INCR_DB *db, char *active);
void incr_commit(INCR_DB *db, const char *active);

#endif

