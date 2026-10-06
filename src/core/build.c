#include "build.h"
#include "cas.h"
#include "deps.h"
#include "graph.h"
#include "hash.h"
#include "incremental.h"
#include "scheduler.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

    GRAPH   graph;
    INCR_DB db;

    char *active;
    char  loaded;

    CAS         *cas;
    ACTION_CACHE ac;
    int          ran;
    int          cached;

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
    buf[got]   = 0;

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

    const char *cache_root = o->cache_dir ? o->cache_dir : o->logdir;

    e->cas = cas_open(cache_root, o->remote, err, errsz);
    ac_init(&e->ac, e->cas, cache_root);
    ac_load(&e->ac);

    return e;
}

void build_close(BUILD_ENGINE *e) {
    if (!e) return;

    ac_save(&e->ac);
    ac_free(&e->ac);
    cas_close(e->cas);

    graph_free(&e->graph);
    incr_free(&e->db);
    free(e->active);
    free(e);
}

typedef struct {
    char              *name;
    unsigned long long hash;
} TOOL_CACHE;

static TOOL_CACHE g_tools[8];
static int        g_ntools = 0;

static unsigned long long tool_resolve(const char *name) {
    if (!name || !name[0]) return 0;

    if (strchr(name, '/')) return hash_read_file(NULL, name);

    const char *path = getenv("PATH");
    if (!path) return 0;

    char *copy = sys_dup(path);
    if (!copy) return 0;

    unsigned long long h    = 0;
    char              *save = NULL;
    char              *dir  = sys_tok(copy, ":", &save);

    while (dir) {
        size_t n    = strlen(dir) + strlen(name) + 2;
        char  *cand = (char *)malloc(n);

        if (cand) {
            snprintf(cand, n, "%s/%s", dir, name);
            h = hash_read_file(NULL, cand);

            free(cand);
            if (h) break;
        }

        dir = sys_tok(NULL, ":", &save);
    }

    free(copy);
    return h;
}

static unsigned long long tool_hash(const char *name) {
    for (int i = 0; i < g_ntools; i++)
        if (!strcmp(g_tools[i].name, name)) return g_tools[i].hash;

    unsigned long long h = tool_resolve(name);

    if (g_ntools < 8) {
        g_tools[g_ntools].name = sys_dup(name);
        g_tools[g_ntools].hash = h;
        g_ntools++;
    }

    return h;
}

static void save_state(BUILD_ENGINE *e) {
    incr_commit(&e->db, e->active);
    hash_db_save(&e->db.files, e->hash_path);
    incr_save(&e->db, e->state_path);
    deps_save(&e->graph, e->deps_path);
}

static int node_last_input(INCR_DB *db, int node, unsigned long long *intrinsic) {
    NODE *nd = &db->g->nodes[node];

    unsigned long long k = nd->cmd_hash;

    for (int i = 0; i < nd->nins; i++) {
        int self = 0;

        for (int j = 0; j < nd->nouts; j++)
            if (!strcmp(nd->outs[j], nd->ins[i])) self = 1;

        if (self) continue;

        k = hash_u64(k, hash_read_file(&db->files, nd->ins[i]));
    }

    *intrinsic = k;
    return 0;
}

static int deps_key(INCR_DB *db, int node, unsigned long long *out) {
    NODE *nd = &db->g->nodes[node];

    unsigned long long k = HASH_FNV_OFFSET;

    for (int i = 0; i < nd->ndyn; i++) k = hash_u64(k, hash_read_file(&db->files, nd->dyn[i]));

    *out = k;
    return 0;
}

static int deps_ready(BUILD_ENGINE *e, int node) {
    NODE *nd = &e->db.g->nodes[node];

    for (int i = 0; i < nd->ndeps; i++)
        if (e->active[nd->deps[i]]) return 0;

    return 1;
}

static void cache_restore(BUILD_ENGINE *e, int *dirty) {
    INCR_DB *db = &e->db;

    for (int i = 0; i < db->n; i++) {
        if (!e->active[i]) continue;

        NODE *nd = &db->g->nodes[i];

        if (nd->nouts == 0) continue;

        if (!deps_ready(e, i)) continue;

        unsigned long long intrinsic = 0;
        unsigned long long dyn       = 0;

        node_last_input(db, i, &intrinsic);
        deps_key(db, i, &dyn);

        unsigned long long key = hash_u64(intrinsic, dyn);
        key                    = hash_u64(key, tool_hash(nd->argv[0]));

        AC_ENTRY *ent = ac_find(&e->ac, key);

        int hit = ent != NULL;

        for (int k = 0; hit && k < ent->nouts; k++)
            if (!cas_has(e->cas, ent->outs[k].hash)) hit = 0;

        if (!hit) continue;

        int restored = 1;

        for (int k = 0; k < ent->nouts; k++)
            if (cas_get_file(e->cas, ent->outs[k].hash, nd->outs[k]) != 0) restored = 0;

        if (!restored) continue;

        SYS_STAT st;

        for (int k = 0; k < ent->nouts; k++)
            if (sys_stat(nd->outs[k], &st) == 0) sys_chmod(nd->outs[k], ent->outs[k].mode);

        incr_record(db, i);
        e->active[i] = 0;
        e->cached++;
        (*dirty)--;
    }
}

static void cache_store(BUILD_ENGINE *e) {
    INCR_DB *db = &e->db;

    for (int i = 0; i < db->n; i++) {
        if (!e->active[i]) continue;

        NODE *nd = &db->g->nodes[i];

        if (nd->nouts == 0) continue;

        unsigned long long intrinsic = 0;
        unsigned long long dyn       = 0;

        node_last_input(db, i, &intrinsic);
        deps_key(db, i, &dyn);

        unsigned long long key = hash_u64(intrinsic, dyn);
        key                    = hash_u64(key, tool_hash(nd->argv[0]));

        AC_OUT *outs = (AC_OUT *)malloc(sizeof(AC_OUT) * (size_t)nd->nouts);
        if (!outs) continue;

        int n  = 0;
        int ok = 1;

        for (int k = 0; k < nd->nouts; k++) {
            SYS_STAT st;

            if (sys_stat(nd->outs[k], &st) != 0) {
                ok = 0;
                break;
            }

            unsigned long long h = hash_read_file(&e->db.files, nd->outs[k]);
            if (!h || cas_put_hashed(e->cas, nd->outs[k], h) != 0) {
                ok = 0;
                break;
            }

            outs[n].hash = h;
            outs[n].mode = st.mode & 07777;
            n++;
        }

        if (ok) ac_put(&e->ac, key, outs, n);

        free(outs);
    }
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

    e->ran    = 0;
    e->cached = 0;

    if (!e->opts.no_cache) cache_restore(e, &dirty);

    if (dirty == 0) {
        save_state(e);
        return 0;
    }

    SCHED_OPTS so;
    so.g          = &e->graph;
    so.cwd        = e->opts.cwd;
    so.logdir     = e->opts.logdir;
    so.jobs       = e->opts.jobs;
    so.retry      = e->opts.retry;
    so.keep_going = e->opts.keep_going;
    so.active     = e->active;
    so.env        = e->opts.env;
    so.nenv       = e->opts.nenv;

    e->ran = dirty;

    int rc = sched_run(&so);

    if (rc == 0) {
        if (!e->opts.no_cache) cache_store(e);

        save_state(e);
    }

    return rc;
}

void build_stats(BUILD_ENGINE *e, int *ran, int *cached) {
    if (ran) *ran = e->ran;
    if (cached) *cached = e->cached;
}
