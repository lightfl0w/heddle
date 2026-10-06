#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void app(char *buf, size_t cap, int *len, const char *fmt, ...) {
    if ((size_t)*len >= cap) return;

    va_list ap;
    va_start(ap, fmt);

    int n = vsnprintf(buf + *len, cap - (size_t)*len, fmt, ap);

    va_end(ap);

    if (n > 0) *len += n;
}

static void put_objs(char *buf, size_t cap, int *len, const LINK_REQ *r, const char *sep) {
    for (int i = 0; i < r->nobj; i++) app(buf, cap, len, "%s%s", i || *len ? sep : "", r->objs[i]);
}

static void put_libs(char *buf, size_t cap, int *len, const LINK_REQ *r, const char *sep) {
    for (int i = 0; i < r->nlib; i++) app(buf, cap, len, "%s%s", *len ? sep : "", r->libs[i]);
}

static void put_flags(char *buf, size_t cap, int *len, const LINK_REQ *r) {
    for (int i = 0; i < r->nldf; i++) app(buf, cap, len, " %s", r->ldflags[i]);
}

static void gnu_cmd(const TOOLCHAIN *tc, const LINK_REQ *r, char *buf, size_t cap, int *len) {
    app(buf, cap, len, "%s", tc->ld);

    if (r->shared) app(buf, cap, len, " %s", tc->soflag);

    app(buf, cap, len, " -o %s", r->out);

    put_objs(buf, cap, len, r, " ");
    put_libs(buf, cap, len, r, " ");

    if (r->entry) app(buf, cap, len, " -e %s", r->entry);

    if (r->ldscript) app(buf, cap, len, " -T %s", r->ldscript);

    put_flags(buf, cap, len, r);
}

static void armcc_cmd(const TOOLCHAIN *tc, const LINK_REQ *r, char *buf, size_t cap, int *len) {
    app(buf, cap, len, "%s --output=%s", tc->ld, r->out);

    if (r->shared) app(buf, cap, len, " --shared");
    if (r->ldscript) app(buf, cap, len, " --scatter=%s", r->ldscript);
    if (r->entry) app(buf, cap, len, " --entry=%s", r->entry);

    put_objs(buf, cap, len, r, " ");
    put_libs(buf, cap, len, r, " ");
    put_flags(buf, cap, len, r);
}

static void iar_cmd(const TOOLCHAIN *tc, const LINK_REQ *r, char *buf, size_t cap, int *len) {
    app(buf, cap, len, "%s --output=%s", tc->ld, r->out);

    if (r->shared) app(buf, cap, len, " --dlib_config shared");
    if (r->ldscript) app(buf, cap, len, " --config=%s", r->ldscript);
    if (r->entry) app(buf, cap, len, " --entry=%s", r->entry);

    put_objs(buf, cap, len, r, " ");
    put_libs(buf, cap, len, r, " ");
    put_flags(buf, cap, len, r);
}

static int msvc_lib_ok(const char *f) {
    if (!strcmp(f, "-nostdlib") || !strcmp(f, "-Wl,--build-id=none")) return 0;

    return f[0] != '-';
}

static void put_msvc_ldflags(char *buf, size_t cap, int *len, const LINK_REQ *r) {
    for (int i = 0; i < r->nldf; i++) {
        const char *f = r->ldflags[i];

        if (!f || !*f) continue;

        if (!strncmp(f, "-Wl,", 4)) app(buf, cap, len, " %s", f + 4);
        else if (msvc_lib_ok(f)) app(buf, cap, len, " %s", f);
    }
}

static void msvc_cmd(const TOOLCHAIN *tc, const LINK_REQ *r, char *buf, size_t cap, int *len) {
    app(buf, cap, len, "%s /nologo", tc->ld);

    if (r->shared) app(buf, cap, len, " /LD");

    put_objs(buf, cap, len, r, " ");
    put_libs(buf, cap, len, r, " ");

    if (r->ldscript) app(buf, cap, len, " /DEF:%s", r->ldscript);
    if (r->entry) app(buf, cap, len, " /ENTRY:%s", r->entry);

    app(buf, cap, len, " /Fe%s /link", r->out);
    put_msvc_ldflags(buf, cap, len, r);
}

void link_cmd(const TOOLCHAIN *tc, const LINK_REQ *r, char *buf, size_t cap) {
    int len = 0;

    buf[0] = 0;

    if (tc->family && !strcmp(tc->family, "armcc")) armcc_cmd(tc, r, buf, cap, &len);
    else if (tc->family && !strcmp(tc->family, "iar")) iar_cmd(tc, r, buf, cap, &len);
    else if (tc->family && !strcmp(tc->family, "msvc")) msvc_cmd(tc, r, buf, cap, &len);
    else gnu_cmd(tc, r, buf, cap, &len);
}
