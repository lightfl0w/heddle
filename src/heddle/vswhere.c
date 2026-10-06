#include "vswhere.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

static int run_capture(const char *cmd, char *out, size_t cap) {
    FILE *f = _popen(cmd, "rb");
    if (!f) return -1;

    size_t got = fread(out, 1, cap - 1, f);

    out[got] = 0;

    int rc = _pclose(f);

    while (got > 0 && (out[got - 1] == '\n' || out[got - 1] == '\r')) out[--got] = 0;

    return rc == 0 ? 0 : -1;
}

int vs_env_capture(const char *install, const char *arch, const char *ver, char *out, size_t cap) {
    char        script[1400];
    char        cmd[2048];
    char        arg[128];
    const char *a = (arch && *arch) ? arch : "x64";

    snprintf(script, sizeof(script), "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat", install);

    if (GetFileAttributesA(script) == INVALID_FILE_ATTRIBUTES) return -1;

    if (ver && *ver) snprintf(arg, sizeof(arg), "%s -vcvars_ver=%s", a, ver);
    else snprintf(arg, sizeof(arg), "%s", a);

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

#else

int vs_install(char *out, size_t cap) {
    (void)out;
    (void)cap;
    return -1;
}

int vs_env_capture(const char *install, const char *arch, const char *ver, char *out, size_t cap) {
    (void)install;
    (void)arch;
    (void)ver;
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
