#include "build.h"
#include "deps.h"
#include "graph.h"
#include "hash.h"
#include "incremental.h"
#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

struct BUILD_ENGINE {
    BUILD_OPTS opts;

    GRAPH  graph;
    INCR_DB db;

    char *active;
    char  loaded;

    char state_path[1024];
    char hash_path[1024];
    char deps_path[1024];
};

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = 0;

    fclose(f);
    return buf;
}

static int graph_load(BUILD_ENGINE *e, char *err, size_t errsz) {
    char *text = read_file(e->opts.graph_file);
    if (!text) {
        snprintf(err, errsz, "cannot read %s", e->opts.graph_file);
        return -1;
    }

    GRAPH g;
    int   rc = graph_parse(text, &g, err, errsz);

    free(text);
    if (rc != 0) return -1;

    graph_free(&e->graph);
    e->graph = g;
    return 0;
}

static void db_load(BUILD_ENGINE *e) {
    if (e->loaded) return;

    hash_db_free(&e->db.files);
    hash_db_init(&e->db.files);
    hash_db_load(&e->db.files, e->hash_path);
    incr_load(&e->db, e->state_path);
    deps_load(&e->graph, e->deps_path);

    e->loaded = 1;
}

BUILD_ENGINE *build_open(const BUILD_OPTS *o, char *err, size_t errsz) {
    BUILD_ENGINE *e = (BUILD_ENGINE *)calloc(1, sizeof(BUILD_ENGINE));
    if (!e) return NULL;

    e->opts = *o;

    if (graph_load(e, err, errsz) != 0) {
        free(e);
        return NULL;
    }

    MKDIR(o->logdir);

    snprintf(e->state_path, sizeof(e->state_path), "%s/loom.db", o->logdir);
    snprintf(e->hash_path, sizeof(e->hash_path), "%s/loom.hash", o->logdir);
    snprintf(e->deps_path, sizeof(e->deps_path), "%s/loom.deps", o->logdir);

    incr_init(&e->db, &e->graph);
    e->active = (char *)calloc((size_t)(e->graph.n ? e->graph.n : 1), 1);

    return e;
}

void build_close(BUILD_ENGINE *e) {
    if (!e) return;

    graph_free(&e->graph);
    incr_free(&e->db);
    free(e->active);
    free(e);
}

int build_run(BUILD_ENGINE *e) {
    db_load(e);

    char err[512] = {0};

    deps_scan(&e->graph, e->opts.cwd, &e->db.files);

    if (graph_finalize(&e->graph, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    int dirty = incr_plan(&e->db, e->active);
    if (dirty == 0) return 0;

    SCHED_OPTS so;
    so.g          = &e->graph;
    so.cwd        = e->opts.cwd;
    so.logdir     = e->opts.logdir;
    so.jobs       = e->opts.jobs;
    so.retry      = e->opts.retry;
    so.keep_going = e->opts.keep_going;
    so.active     = e->active;

    int rc = sched_run(&so);

    if (rc == 0) {
        incr_commit(&e->db, e->active);
        hash_db_save(&e->db.files, e->hash_path);
        incr_save(&e->db, e->state_path);
        deps_save(&e->graph, e->deps_path);
    }

    return rc;
}
