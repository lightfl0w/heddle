#ifndef TOML_H
#define TOML_H

#include <stddef.h>

typedef struct {
    char *key;
    char *val;
} TOML_KV;

typedef struct {
    char    *name;
    TOML_KV *items;
    int      count;
    int      cap;
} TOML_TABLE;

typedef struct {
    TOML_TABLE *tables;
    int         count;
    int         cap;
} TOML;

void toml_init(TOML *t);

void toml_free(TOML *t);

int toml_parse(TOML *t, const char *path, char *err, size_t errsz);

const char *toml_str(const TOML *t, const char *section, const char *key);

const char *toml_arr(const TOML *t, const char *section, const char *key,
                     int index);

int toml_int(const TOML *t, const char *section, const char *key,
             int fallback);

int toml_bool(const TOML *t, const char *section, const char *key,
              int fallback);

int toml_sections(const TOML *t, const char *prefix, char ***out);

#endif
