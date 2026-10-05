#ifndef SYS_H
#define SYS_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

typedef struct {
    long long size;
    long long mtime_ns;
    unsigned  mode;
    int       is_dir;
} SYS_STAT;

int  sys_stat(const char *path, SYS_STAT *out);
int  sys_isdir(const char *path);
void sys_chmod(const char *path, unsigned mode);
void sys_mkpath(const char *path);
int  sys_chdir(const char *path);

char *sys_dup(const char *s);
char *sys_tok(char *buf, const char *delims, char **save);

#endif
