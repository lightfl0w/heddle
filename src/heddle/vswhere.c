#include "vswhere.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static const char *skip_bom(const char *s) {
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        return s + 3;

    return s;
}

char *vs_json_str(const char *json, const char *key) {
    char pat[128];

    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(skip_bom(json), pat);
    if (!p) return NULL;

    p = strchr(p + strlen(pat), ':');
    if (!p) return NULL;

    while (*p && *p != '"' && *p != '\n') p++;
    if (*p != '"') return NULL;

    p++;

    char  *out = (char *)malloc(strlen(p) + 1);
    size_t n   = 0;

    if (!out) return NULL;

    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            out[n++] = (*p == 'n') ? '\n' : *p;
            p++;
            continue;
        }

        out[n++] = *p++;
    }

    out[n] = 0;
    return out;
}

int vs_ver_cmp(const char *a, const char *b) {
    for (;;) {
        while (*a == '.') a++;
        while (*b == '.') b++;

        if (!*a || !*b) break;

        long x = strtol(a, (char **)&a, 10);
        long y = strtol(b, (char **)&b, 10);

        if (x != y) return x < y ? -1 : 1;
    }

    return 0;
}

#if defined(_WIN32)

static int run_capture(const char *cmd, char *out, size_t cap) {
    FILE *f = _popen(cmd, "rb");
    if (!f) return -1;

    size_t got = fread(out, 1, cap - 1, f);

    out[got] = 0;

    int rc = _pclose(f);

    while (got > 0 && (out[got - 1] == '\n' || out[got - 1] == '\r')) out[--got] = 0;

    return rc == 0 ? 0 : -1;
}

int vs_env_capture(const char *install, const char *arch, char *out, size_t cap) {
    char script[1400];
    char cmd[2048];
    char arg[64];

    snprintf(arg, sizeof(arg), "%s", (arch && *arch) ? arch : "x64");
    snprintf(script, sizeof(script), "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat", install);

    if (GetFileAttributesA(script) == INVALID_FILE_ATTRIBUTES) return -1;

    snprintf(cmd, sizeof(cmd), "cmd /d /s /c \"\"%s\" %s >nul 2>nul && set\"", script, arg);

    if (run_capture(cmd, out, cap) != 0) return -1;

    if (!out[0]) return -1;

    return 0;
}

int vs_env_split(char *buf, char **items, int maxitems) {
    int   n    = 0;
    char *line = buf;

    while (line && *line && n < maxitems) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r')) line[--len] = 0;
        if (line[0]) items[n++] = line;

        line = nl ? nl + 1 : NULL;
    }

    return n;
}

static int newest_dir(const char *base, const char *want, char *out, size_t cap) {
    char pat[1400];

    snprintf(pat, sizeof(pat), "%s\\*", base);

    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA(pat, &fd);

    if (h == INVALID_HANDLE_VALUE) return -1;

    char best[260] = {0};

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (want && *want && strncmp(fd.cFileName, want, strlen(want))) continue;

        if (vs_ver_cmp(fd.cFileName, best) > 0) snprintf(best, sizeof(best), "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));

    FindClose(h);

    if (!best[0]) return -1;

    snprintf(out, cap, "%s\\%s", base, best);
    return 0;
}

int vs_install(char *out, size_t cap) {
    const char *envs[]   = {"ProgramFiles(x86)", "ProgramFiles"};
    char        vs[1024] = {0};

    for (int i = 0; i < 2 && !vs[0]; i++) {
        const char *root = getenv(envs[i]);

        if (!root) continue;

        snprintf(vs, sizeof(vs), "%s\\Microsoft Visual Studio\\Installer\\vswhere.exe", root);

        if (GetFileAttributesA(vs) == INVALID_FILE_ATTRIBUTES) vs[0] = 0;
    }

    if (!vs[0]) return -1;

    char cmd[2048];

    snprintf(cmd, sizeof(cmd),
             "\"%s\" -latest -products * -prerelease "
             "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
             "-property installationPath",
             vs);

    return run_capture(cmd, out, cap);
}

int vs_toolset(const char *install, const char *want, char *out, size_t cap) {
    char base[1200];

    snprintf(base, sizeof(base), "%s\\VC\\Tools\\MSVC", install);

    return newest_dir(base, want, out, cap);
}

int vs_sdk(const char *arch, char *inc, size_t icap, char *lib, size_t lcap) {
    char  root[1024] = {0};
    DWORD n          = sizeof(root);
    HKEY  k          = NULL;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots", 0,
                      KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS)
        return -1;

    RegQueryValueExA(k, "KitsRoot10", NULL, NULL, (BYTE *)root, &n);
    RegCloseKey(k);

    if (!root[0]) return -1;

    char inc_root[1100];
    char ver[1200];

    snprintf(inc_root, sizeof(inc_root), "%sInclude", root);

    if (newest_dir(inc_root, NULL, ver, sizeof(ver)) != 0) return -1;

    const char *v = strrchr(ver, '\\') + 1;

    snprintf(inc, icap,
             "%s\\Include\\%s\\ucrt;%s\\Include\\%s\\um;"
             "%s\\Include\\%s\\shared",
             root, v, root, v, root, v);

    snprintf(lib, lcap, "%s\\Lib\\%s\\ucrt\\%s;%s\\Lib\\%s\\um\\%s", root, v, arch, root, v, arch);

    return 0;
}

#else

int vs_install(char *out, size_t cap) {
    (void)out;
    (void)cap;
    return -1;
}

int vs_toolset(const char *install, const char *want, char *out, size_t cap) {
    (void)install;
    (void)want;
    (void)out;
    (void)cap;
    return -1;
}

int vs_sdk(const char *arch, char *inc, size_t icap, char *lib, size_t lcap) {
    (void)arch;
    (void)inc;
    (void)icap;
    (void)lib;
    (void)lcap;
    return -1;
}

int vs_env_capture(const char *install, const char *arch, char *out, size_t cap) {
    (void)install;
    (void)arch;
    (void)out;
    (void)cap;
    return -1;
}

int vs_env_split(char *buf, char **items, int maxitems) {
    (void)buf;
    (void)items;
    (void)maxitems;
    return 0;
}

#endif
