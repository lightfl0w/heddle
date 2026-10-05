#ifndef CAS_H
#define CAS_H

#include <stddef.h>

typedef struct CAS CAS;

typedef struct {
    unsigned long long hash;
    unsigned int       mode;
} AC_OUT;

typedef struct {
    unsigned long long key;
    AC_OUT            *outs;
    int                nouts;
} AC_ENTRY;

typedef struct {
    AC_ENTRY *items;
    int       count;
    int       cap;

    CAS  *cas;
    char *root;
} ACTION_CACHE;

CAS *cas_open(const char *root, const char *remote, char *err, size_t errsz);
void cas_close(CAS *c);
int cas_has(CAS *c, unsigned long long h);
int cas_put_bytes(CAS *c, unsigned long long h, const void *p, size_t n);
int cas_get_bytes(CAS *c, unsigned long long h, void **out, size_t *outlen);
unsigned long long cas_put_file(CAS *c, const char *path);
int cas_put_hashed(CAS *c, const char *path, unsigned long long h);
int cas_get_file(CAS *c, unsigned long long h, const char *path);
void ac_init(ACTION_CACHE *ac, CAS *cas, const char *root);
void ac_free(ACTION_CACHE *ac);
int ac_load(ACTION_CACHE *ac);
int ac_save(const ACTION_CACHE *ac);
AC_ENTRY *ac_find(ACTION_CACHE *ac, unsigned long long key);
void ac_put(ACTION_CACHE *ac, unsigned long long key,
            const AC_OUT *outs, int nouts);

#endif
