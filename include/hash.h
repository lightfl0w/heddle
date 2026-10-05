#ifndef HASH_H
#define HASH_H

#include <stddef.h>

#define HASH_FNV_OFFSET 1469598103934665603ULL
#define HASH_FNV_PRIME  1099511628211ULL

typedef struct {
    char              *path;
    unsigned long long value;
    long long          mtime_ns;
    long long          size;
} HASH_ENTRY;

typedef struct {
    HASH_ENTRY *items;
    int         count;
    int         cap;
} HASH_DB;

void hash_db_init(HASH_DB *db);
void hash_db_free(HASH_DB *db);
unsigned long long hash_bytes(unsigned long long seed, const void *p, size_t n);
unsigned long long hash_text(unsigned long long seed, const char *s);
unsigned long long hash_u64(unsigned long long seed, unsigned long long v);
unsigned long long hash_read_file(HASH_DB *db, const char *path);
int hash_db_load(HASH_DB *db, const char *path);
int hash_db_save(const HASH_DB *db, const char *path);
HASH_ENTRY *hash_db_find(HASH_DB *db, const char *path);

#endif

