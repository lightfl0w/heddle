#ifndef VSWHERE_H
#define VSWHERE_H

#include <stddef.h>

int vs_install(char *out, size_t cap);

int vs_toolset(const char *install, const char *want, char *out, size_t cap);

int vs_sdk(const char *arch, char *inc, size_t icap, char *lib, size_t lcap);

char *vs_json_str(const char *json, const char *key);

int vs_ver_cmp(const char *a, const char *b);

#endif
