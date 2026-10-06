#ifndef HEDDLE_JSON_H
#define HEDDLE_JSON_H

#include <stddef.h>

typedef struct {
    char *key;
    char *val;
} JSON_KV;

typedef struct {
    JSON_KV *items;
    int      n;
    int      cap;
} JSON;

void json_init(JSON *j);
void json_free(JSON *j);

int json_parse(JSON *j, const char *path, char *err, size_t errsz);

const char *json_get(const JSON *j, const char *key);

#endif
