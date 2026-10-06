#include "sys.h"

#if !defined(_WIN32)
static long long st_ns(const struct stat *st) {
#if defined(__APPLE__)
    return (long long)st->st_mtimespec.tv_nsec;
#elif defined(_POSIX_C_SOURCE) || defined(_GNU_SOURCE) || defined(_DEFAULT_SOURCE)
    return (long long)st->st_mtim.tv_nsec;
#else
    return 0;
#endif
}
#endif

#if defined(_WIN32)

#include <direct.h>
#include <io.h>

void sys_chmod(const char *path, unsigned mode) {
    _chmod(path, (int)(mode & 0777));
}

int sys_stat(const char *path, SYS_STAT *out) {
    struct _stat64 st;

    if (_stat64(path, &st) != 0) return -1;

    out->size     = (long long)st.st_size;
    out->mtime_ns = (long long)st.st_mtime * 1000000000LL;
    out->mode     = (unsigned)st.st_mode;
    out->is_dir   = (st.st_mode & _S_IFDIR) != 0;
    return 0;
}

int sys_isdir(const char *path) {
    SYS_STAT st;
    return sys_stat(path, &st) == 0 && st.is_dir;
}

#else

#include <sys/stat.h>
#include <unistd.h>

void sys_chmod(const char *path, unsigned mode) {
    chmod(path, (mode_t)mode);
}

int sys_stat(const char *path, SYS_STAT *out) {
    struct stat st;

    if (stat(path, &st) != 0) return -1;

    out->size     = (long long)st.st_size;
    out->mtime_ns = (long long)st.st_mtime * 1000000000LL + st_ns(&st);
    out->mode     = (unsigned)st.st_mode;
    out->is_dir   = S_ISDIR(st.st_mode);
    return 0;
}

int sys_isdir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

#endif

char *sys_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char  *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);

    return p;
}

int sys_chdir(const char *path) {
#if defined(_WIN32)
    return _chdir(path);
#else
    return chdir(path);
#endif
}

void sys_mkpath(const char *path) {
    char *tmp = sys_dup(path);
    if (!tmp) return;

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;

        *p = 0;
        MKDIR(tmp);
        *p = '/';
    }

    MKDIR(tmp);
    free(tmp);
}

char *sys_tok(char *buf, const char *delims, char **save) {
    if (!buf) buf = *save;
    if (!buf) return NULL;

    buf += strspn(buf, delims);
    if (*buf == 0) {
        *save = buf;
        return NULL;
    }

    char *tok = buf;
    buf += strcspn(buf, delims);
    if (*buf) {
        *buf = 0;
        buf++;
    }

    *save = buf;
    return tok;
}
