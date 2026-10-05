
#include "hash.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __APPLE__
#include <sys/time.h>
#endif

void hash_db_init(HASH_DB *db) {
    db->items = NULL;
    db->count = 0;
    db->cap   = 0;
}

void hash_db_free(HASH_DB *db) {
    for (int i = 0; i < db->count; i++)
        free(db->items[i].path);

    free(db->items);
    hash_db_init(db);
}

unsigned long long hash_bytes(unsigned long long seed, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    unsigned long long   h = seed;

    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= HASH_FNV_PRIME;
    }

    return h;
}

unsigned long long hash_text(unsigned long long seed, const char *s) {
    return hash_bytes(seed, s, strlen(s) + 1);
}

unsigned long long hash_u64(unsigned long long seed, unsigned long long v) {
    return hash_bytes(seed, &v, sizeof(v));
}

HASH_ENTRY *hash_db_find(HASH_DB *db, const char *path) {
    for (int i = 0; i < db->count; i++)
        if (!strcmp(db->items[i].path, path)) return &db->items[i];

    return NULL;
}

static HASH_ENTRY *db_insert(HASH_DB *db, const char *path) {
    if (db->count == db->cap) {
        int newcap = db->cap ? db->cap * 2 : 64;

        HASH_ENTRY *items = (HASH_ENTRY *)realloc(
            db->items, sizeof(HASH_ENTRY) * (size_t)newcap);
        if (!items) return NULL;

        db->items = items;
        db->cap   = newcap;
    }

    char *copy = sys_dup(path);
    if (!copy) return NULL;

    HASH_ENTRY *e = &db->items[db->count++];

    e->path     = copy;
    e->value    = 0;
    e->mtime_ns = 0;
    e->size     = 0;
    return e;
}

unsigned long long hash_read_file(HASH_DB *db, const char *path) {
    SYS_STAT st;
    if (sys_stat(path, &st) != 0) return 0;

    long long mt   = st.mtime_ns;
    long long size = st.size;

    HASH_ENTRY *e = db ? hash_db_find(db, path) : NULL;
    if (e && e->value && e->mtime_ns == mt && e->size == size)
        return e->value;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    char               buf[65536];
    unsigned long long h = HASH_FNV_OFFSET;
    size_t             got;

    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        h = hash_bytes(h, buf, got);

    fclose(f);

    if (db) {
        if (!e) e = db_insert(db, path);

        if (e) {
            e->value    = h;
            e->mtime_ns = mt;
            e->size     = size;
        }
    }

    return h;
}

int hash_db_load(HASH_DB *db, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    char line[8192];

    while (fgets(line, sizeof(line), f)) {
        char *save = NULL;
        char *tag  = sys_tok(line, " \t\r\n", &save);

        if (!tag || strcmp(tag, "H")) continue;

        char *hex   = sys_tok(NULL, " \t\r\n", &save);
        char *mtime = sys_tok(NULL, " \t\r\n", &save);
        char *size  = sys_tok(NULL, " \t\r\n", &save);
        char *file  = sys_tok(NULL, "\r\n", &save);

        if (!hex || !mtime || !size || !file) continue;

        HASH_ENTRY *e = db_insert(db, file);
        if (!e) {
            fclose(f);
            return -1;
        }

        e->value    = strtoull(hex, NULL, 16);
        e->mtime_ns = strtoll(mtime, NULL, 10);
        e->size     = strtoll(size, NULL, 10);
    }

    fclose(f);
    return 0;
}

int hash_db_save(const HASH_DB *db, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    for (int i = 0; i < db->count; i++)
        fprintf(f, "H %016llx %lld %lld %s\n",
                db->items[i].value, db->items[i].mtime_ns,
                db->items[i].size, db->items[i].path);

    fclose(f);
    return 0;
}
