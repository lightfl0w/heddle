#ifndef HEDDLE_VCPKG_H
#define HEDDLE_VCPKG_H

#include <stddef.h>

typedef struct {
    char  *name;
    char  *version;
    char  *desc;
    char  *homepage;
    char **deps;
    int    ndeps;
    char  *dir;
} VCPKG_PORT;

int  vcpkg_port_load(VCPKG_PORT *v, const char *dir, char *err, size_t errsz);
void vcpkg_port_free(VCPKG_PORT *v);

int  vcpkg_triplet(const char *triplet, char *arch, size_t acap,
                   char *abi, size_t bcap, char *flt, size_t fcap);

int  vcpkg_import(const VCPKG_PORT *v, const char *installed,
                  const char *triplet, const char *store,
                  char *hash, size_t hcap, char *err, size_t errsz);

int  vcpkg_is_registry(const char *dir);

int  heddle_vcpkg(int argc, char **argv);

#endif
