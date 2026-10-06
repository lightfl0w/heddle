#ifndef VSWHERE_H
#define VSWHERE_H

#include <stddef.h>

int vs_install(char *out, size_t cap);

int vs_env_capture(const char *install, const char *arch, const char *ver,
                   char *out, size_t cap);

int vs_env_split(char *buf, char **items, int maxitems);

#endif
