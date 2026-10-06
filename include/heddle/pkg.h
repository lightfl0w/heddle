#ifndef HEDDLE_PKG_H
#define HEDDLE_PKG_H

#include <stddef.h>

#include "toolchain.h"

typedef enum {
    PKG_KIND_TOOLCHAIN,
    PKG_KIND_LIBRARY
} PKG_KIND;

typedef enum {
    PKG_SOURCE_AUTO,
    PKG_SOURCE_SOURCE,
    PKG_SOURCE_BINARY
} PKG_SOURCE;

typedef struct {
    char       *name;
    char       *version;
    char       *target;
    PKG_KIND    kind;
    PKG_SOURCE  source;
    char       *hash;
    char       *store_path;
    int         recipe;
} PKG_SPEC;

typedef struct {
    PKG_SPEC *items;
    int       n;
    int       cap;
} PKG_LIST;

typedef struct {
    char *arch;
    char *abi;
    char *float_k;
    char *prefix;
    char *sysroot;
    char *cpu;
    char *fpu;
} TARGET_PROFILE;

typedef struct {
    char *root;
    char *store;
    char *lock_path;
    char *manifest;
    char *toolchain_prefix;
    char *sysroot;
    PKG_LIST tools;
    PKG_LIST deps;
    TARGET_PROFILE target;
} PKG_MANIFEST;

int  pkg_manifest_load(PKG_MANIFEST *m, const char *root, char *err, size_t errsz);
void pkg_manifest_free(PKG_MANIFEST *m);
int  pkg_target_resolve(TARGET_PROFILE *t, char *err, size_t errsz);
int  pkg_install(PKG_MANIFEST *m, int offline, int verbose, char *err, size_t errsz);
int  pkg_verify(PKG_MANIFEST *m, int verbose, char *err, size_t errsz);
void pkg_plan(const PKG_MANIFEST *m);
char **pkg_env(const PKG_MANIFEST *m, int *out_n);
int  pkg_prepend_path(PKG_MANIFEST *m);

char *pkg_variant_key(const TARGET_PROFILE *t, const TOOLCHAIN *tc,
                      char *out, size_t cap);

void pkg_hash_tree(const char *dir, char *out, size_t cap);
int  pkg_copy_tree(const char *src, const char *dst);
int  pkg_lock_add(const char *lock, const char *kind, const char *name,
                  const char *version, const char *hash, char *err, size_t errsz);

#endif
