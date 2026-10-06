#include "incremental.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void incr_init(INCR_DB *db, const GRAPH *g) {
    db->g      = g;
    db->n      = g->n;
    db->states = (NODE_STATE *)calloc((size_t)(db->n ? db->n : 1), sizeof(NODE_STATE));
    hash_db_init(&db->files);
}

void incr_free(INCR_DB *db) {
    free(db->states);
    hash_db_free(&db->files);
}

static unsigned long long node_input_hash(const NODE *nd, HASH_DB *files) {
    unsigned long long h = nd->cmd_hash;
    for (int i = 0; i < nd->nins; i++) h = hash_u64(h, hash_read_file(files, nd->ins[i]));

    for (int i = 0; i < nd->ndyn; i++) h = hash_u64(h, hash_read_file(files, nd->dyn[i]));

    return h;
}

static unsigned long long node_output_hash(const NODE *nd, HASH_DB *files) {
    unsigned long long h = HASH_FNV_OFFSET;
    for (int i = 0; i < nd->nouts; i++) h = hash_u64(h, hash_read_file(files, nd->outs[i]));

    return h;
}

int incr_load(INCR_DB *db, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    int magic = 0;
    int n     = 0;
    if (fread(&magic, sizeof(int), 1, f) != 1 || fread(&n, sizeof(int), 1, f) != 1 ||
        magic != 0x4c4f4f4d || n != db->n) {
        fclose(f);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        if (fread(&db->states[i], sizeof(NODE_STATE), 1, f) != 1) {
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return 1;
}

int incr_save(const INCR_DB *db, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int magic = 0x4c4f4f4d;
    fwrite(&magic, sizeof(int), 1, f);
    fwrite(&db->n, sizeof(int), 1, f);
    for (int i = 0; i < db->n; i++) fwrite(&db->states[i], sizeof(NODE_STATE), 1, f);

    fclose(f);
    return 0;
}

static int node_dirty(INCR_DB *db, const NODE *nd, const NODE_STATE *st) {
    if (!st->valid) return 1;
    if (st->cmd_hash != nd->cmd_hash) return 1;
    if (st->in_hash != node_input_hash(nd, &db->files)) return 1;
    if (st->out_hash != node_output_hash(nd, &db->files)) return 1;
    return 0;
}

int incr_plan(INCR_DB *db, char *active) {
    const GRAPH *g     = db->g;
    int          count = 0;
    for (int i = 0; i < db->n; i++) active[i] = (char)node_dirty(db, &g->nodes[i], &db->states[i]);

    for (int i = 0; i < db->n; i++) {
        if (!active[i]) continue;

        count++;
        for (int k = 0; k < g->nodes[i].nrdeps; k++) active[g->nodes[i].rdeps[k]] = 1;
    }

    return count;
}

void incr_record(INCR_DB *db, int node) {
    const NODE *nd            = &db->g->nodes[node];
    db->states[node].cmd_hash = nd->cmd_hash;
    db->states[node].in_hash  = node_input_hash(nd, &db->files);
    db->states[node].out_hash = node_output_hash(nd, &db->files);
    db->states[node].valid    = 1;
}

void incr_commit(INCR_DB *db, const char *active) {
    const GRAPH *g = db->g;
    for (int i = 0; i < db->n; i++) {
        if (!active[i]) continue;

        const NODE *nd         = &g->nodes[i];
        db->states[i].cmd_hash = nd->cmd_hash;
        db->states[i].in_hash  = node_input_hash(nd, &db->files);
        db->states[i].out_hash = node_output_hash(nd, &db->files);
        db->states[i].valid    = 1;
    }
}
