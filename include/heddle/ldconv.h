#ifndef HEDDLE_LDCONV_H
#define HEDDLE_LDCONV_H

#include <stddef.h>

int ldconv_convert(const char *gnu_ld, const char *family,
                   const char *out_path, char *err, size_t errsz);

#endif
