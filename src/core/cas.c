#include "cas.h"
#include "hash.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

struct CAS {
    char *root;
    char *remote;
    int   remote_http;
};

static void make_dirs(const char *path) {
    char *tmp = sys_dup(path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;

        *p = 0;
        MKDIR(tmp);
        *p = '/';
    }

    MKDIR(tmp);
    free(tmp);
}

static void obj_rel(unsigned long long h, char *buf, size_t n) {
    snprintf(buf, n, "objects/%02llx/%014llx",
             h & 0xffULL, h >> 8);
}

static int try_reflink(int in, int out) {
#ifdef __linux__
#ifndef FICLONE
#define FICLONE 0x40049409
#endif
    return ioctl(out, FICLONE, in) == 0;
#else
    (void)in;
    (void)out;
    return 0;
#endif
}

static int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY);
    if (in < 0) return -1;

    char *dir = sys_dup(to);
    char *slash = strrchr(dir, '/');

    if (slash) {
        *slash = 0;
        make_dirs(dir);
    }

    free(dir);

    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }

    if (try_reflink(in, out)) {
        close(in);
        close(out);
        return 0;
    }

    char   buf[65536];
    long long got;

    while ((got = read(in, buf, sizeof(buf))) > 0) {
        long long off = 0;

        while (off < got) {
            long long n = (long long)write(out, buf + off, (size_t)(got - off));
            if (n <= 0) {
                close(in);
                close(out);
                return -1;
            }
            off += n;
        }
    }

    close(in);
    close(out);
    return 0;
}

static int copy_bytes(const void *p, size_t n, const char *to) {
    char *dir = sys_dup(to);
    char *slash = strrchr(dir, '/');

    if (slash) {
        *slash = 0;
        make_dirs(dir);
    }

    free(dir);

    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) return -1;

    const char *b = (const char *)p;
    size_t off = 0;

    while (off < n) {
        long long w = (long long)write(out, b + off, (size_t)(n - off));
        if (w <= 0) {
            close(out);
            return -1;
        }
        off += (size_t)w;
    }

    close(out);
    return 0;
}

static int remote_fetch(CAS *c, const char *rel, const char *dest) {
    if (!c->remote) return -1;

    if (c->remote_http) {
        size_t n = strlen(c->remote) + strlen(rel) + 16;
        char  *url = (char *)malloc(n);
        if (!url) return -1;

        snprintf(url, n, "%s/%s", c->remote, rel);

        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "curl -fsS -o '%s' '%s' >/dev/null 2>&1",
                 dest, url);

        int rc = system(cmd);

        free(url);
        return rc == 0 ? 0 : -1;
    }

    size_t n = strlen(c->remote) + strlen(rel) + 2;
    char  *src = (char *)malloc(n);
    if (!src) return -1;

    snprintf(src, n, "%s/%s", c->remote, rel);

    int rc = copy_file(src, dest);

    free(src);
    return rc;
}

static int remote_push(CAS *c, const char *rel, const char *src) {
    if (!c->remote) return -1;

    if (c->remote_http) {
        size_t n = strlen(c->remote) + strlen(rel) + 16;
        char  *url = (char *)malloc(n);
        if (!url) return -1;

        snprintf(url, n, "%s/%s", c->remote, rel);

        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "curl -fsS -T '%s' '%s' >/dev/null 2>&1",
                 src, url);

        int rc = system(cmd);

        free(url);
        return rc == 0 ? 0 : -1;
    }

    size_t n = strlen(c->remote) + strlen(rel) + 2;
    char  *dst = (char *)malloc(n);
    if (!dst) return -1;

    snprintf(dst, n, "%s/%s", c->remote, rel);

    SYS_STAT st;
    if (sys_stat(dst, &st) == 0) {
        free(dst);
        return 0;
    }

    int rc = copy_file(src, dst);

    free(dst);
    return rc;
}

CAS *cas_open(const char *root, const char *remote, char *err, size_t errsz) {
    CAS *c = (CAS *)calloc(1, sizeof(CAS));
    if (!c) return NULL;

    c->root   = sys_dup(root);
    c->remote = remote ? sys_dup(remote) : NULL;

    if (c->remote)
        c->remote_http = !strncmp(c->remote, "http://", 7) ||
                         !strncmp(c->remote, "https://", 8);

    make_dirs(root);

    (void)err;
    (void)errsz;
    return c;
}

void cas_close(CAS *c) {
    if (!c) return;

    free(c->root);
    free(c->remote);
    free(c);
}

static void obj_path(CAS *c, unsigned long long h, char *buf, size_t n) {
    char rel[64];
    obj_rel(h, rel, sizeof(rel));
    snprintf(buf, n, "%s/%s", c->root, rel);
}

int cas_has(CAS *c, unsigned long long h) {
    char path[2048];
    obj_path(c, h, path, sizeof(path));

    SYS_STAT st;
    if (sys_stat(path, &st) == 0) return 1;

    char rel[64];
    obj_rel(h, rel, sizeof(rel));

    if (remote_fetch(c, rel, path) == 0) return 1;

    return 0;
}

unsigned long long cas_put_file(CAS *c, const char *path) {
    unsigned long long h = hash_read_file(NULL, path);
    if (!h) return 0;

    cas_put_hashed(c, path, h);
    return h;
}

int cas_put_hashed(CAS *c, const char *path, unsigned long long h) {
    if (!h) return -1;

    char dst[2048];
    obj_path(c, h, dst, sizeof(dst));

    SYS_STAT st;
    if (sys_stat(dst, &st) == 0) return 0;

    copy_file(path, dst);

    char rel[64];
    obj_rel(h, rel, sizeof(rel));
    remote_push(c, rel, dst);

    return 0;
}

int cas_get_file(CAS *c, unsigned long long h, const char *path) {
    char src[2048];
    obj_path(c, h, src, sizeof(src));

    SYS_STAT st;
    if (sys_stat(src, &st) != 0 && !cas_has(c, h)) return -1;

    return copy_file(src, path);
}

int cas_put_bytes(CAS *c, unsigned long long h, const void *p, size_t n) {
    char dst[2048];
    obj_path(c, h, dst, sizeof(dst));

    SYS_STAT st;
    if (sys_stat(dst, &st) == 0) return 0;

    if (copy_bytes(p, n, dst) != 0) return -1;

    char rel[64];
    obj_rel(h, rel, sizeof(rel));
    remote_push(c, rel, dst);

    return 0;
}

int cas_get_bytes(CAS *c, unsigned long long h, void **out, size_t *outlen) {
    char src[2048];
    obj_path(c, h, src, sizeof(src));

    SYS_STAT st;
    if (sys_stat(src, &st) != 0 && !cas_has(c, h)) return -1;
    if (sys_stat(src, &st) != 0) return -1;

    FILE *f = fopen(src, "rb");
    if (!f) return -1;

    char *buf = (char *)malloc((size_t)st.size + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t got = fread(buf, 1, (size_t)st.size, f);
    buf[got] = 0;

    fclose(f);

    *out    = buf;
    *outlen = got;
    return 0;
}

void ac_init(ACTION_CACHE *ac, CAS *cas, const char *root) {
    memset(ac, 0, sizeof(*ac));
    ac->cas  = cas;
    ac->root = sys_dup(root);
}

void ac_free(ACTION_CACHE *ac) {
    for (int i = 0; i < ac->count; i++) {
        free(ac->items[i].outs);
    }

    free(ac->items);
    free(ac->root);
    memset(ac, 0, sizeof(*ac));
}

AC_ENTRY *ac_find(ACTION_CACHE *ac, unsigned long long key) {
    for (int i = 0; i < ac->count; i++)
        if (ac->items[i].key == key) return &ac->items[i];

    return NULL;
}

void ac_put(ACTION_CACHE *ac, unsigned long long key,
            const AC_OUT *outs, int nouts) {
    AC_ENTRY *e = ac_find(ac, key);

    if (!e) {
        if (ac->count == ac->cap) {
            int newcap = ac->cap ? ac->cap * 2 : 64;

            AC_ENTRY *items = (AC_ENTRY *)realloc(
                ac->items, sizeof(AC_ENTRY) * (size_t)newcap);
            if (!items) return;

            ac->items = items;
            ac->cap   = newcap;
        }

        e = &ac->items[ac->count++];
        e->key   = key;
        e->outs  = NULL;
        e->nouts = 0;
    } else {
        free(e->outs);
    }

    e->outs = (AC_OUT *)malloc(sizeof(AC_OUT) * (size_t)(nouts ? nouts : 1));
    if (!e->outs) {
        e->nouts = 0;
        return;
    }

    memcpy(e->outs, outs, sizeof(AC_OUT) * (size_t)nouts);
    e->nouts = nouts;
}

int ac_load(ACTION_CACHE *ac) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/actions", ac->root);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[8192];

    while (fgets(line, sizeof(line), f)) {
        char *save = NULL;
        char *tag  = sys_tok(line, " \t\r\n", &save);

        if (!tag || strcmp(tag, "A")) continue;

        char *key = sys_tok(NULL, " \t\r\n", &save);
        char *n   = sys_tok(NULL, " \t\r\n", &save);

        if (!key || !n) continue;

        unsigned long long k = strtoull(key, NULL, 16);
        int count = atoi(n);

        AC_OUT *outs = (AC_OUT *)malloc(sizeof(AC_OUT) * (size_t)(count ? count : 1));
        if (!outs) break;

        int got = 0;

        for (int i = 0; i < count; i++) {
            char *tok = sys_tok(NULL, " \t\r\n", &save);
            if (!tok) break;

            char *colon = strchr(tok, ':');
            if (colon) *colon = 0;

            outs[got].hash = strtoull(tok, NULL, 16);
            outs[got].mode = colon ? (unsigned int)strtoul(colon + 1, NULL, 8) : 0644;
            got++;
        }

        if (got == count) ac_put(ac, k, outs, got);

        free(outs);
    }

    fclose(f);
    return 0;
}

int ac_save(const ACTION_CACHE *ac) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/actions", ac->root);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    for (int i = 0; i < ac->count; i++) {
        fprintf(f, "A %016llx %d", ac->items[i].key, ac->items[i].nouts);

        for (int k = 0; k < ac->items[i].nouts; k++)
            fprintf(f, " %016llx:%o", ac->items[i].outs[k].hash,
                    ac->items[i].outs[k].mode);

        fputc('\n', f);
    }

    fclose(f);
    return 0;
}
