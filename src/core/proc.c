#include "proc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

static void append_arg(char *buf, size_t cap, size_t *len, const char *arg) {
    int need = 0;

    for (const char *p = arg; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '"') {
            need = 1;
            break;
        }

    if (!need) {
        size_t n = strlen(arg);

        if (*len + n + 1 < cap) {
            memcpy(buf + *len, arg, n);
            *len += n;
            buf[*len] = 0;
        }

        return;
    }

    if (*len + 1 < cap) buf[(*len)++] = '"';

    for (const char *p = arg; *p; p++) {
        if (*p == '"' && *len + 2 < cap) buf[(*len)++] = '\\';
        if (*len + 1 < cap) buf[(*len)++] = *p;
    }

    if (*len + 1 < cap) buf[(*len)++] = '"';
    if (*len < cap) buf[*len] = 0;
}

static int env_name_eq(const char *a, const char *b) {
    while (*a && *b && *a != '=' && *b != '=') {
        char x = (char)toupper((unsigned char)*a++);
        char y = (char)toupper((unsigned char)*b++);

        if (x != y) return 0;
    }

    return *a == '=' && *b == '=';
}

static int env_over(char *const *env, int nenv, const char *entry) {
    for (int i = 0; i < nenv; i++)
        if (env_name_eq(entry, env[i])) return 1;

    return 0;
}

static char *env_merge(char *const *env, int nenv) {
    char  *parent = GetEnvironmentStringsA();
    size_t n = 1;

    for (char *p = parent; *p; p += strlen(p) + 1)
        if (!env_over(env, nenv, p)) n += strlen(p) + 1;

    for (int i = 0; i < nenv; i++)
        n += strlen(env[i]) + 1;

    char *block = (char *)calloc(n + 1, 1);
    if (!block) {
        FreeEnvironmentStringsA(parent);
        return NULL;
    }

    char *w = block;

    for (char *p = parent; *p; p += strlen(p) + 1) {
        if (env_over(env, nenv, p)) continue;

        memcpy(w, p, strlen(p) + 1);
        w += strlen(p) + 1;
    }

    FreeEnvironmentStringsA(parent);

    for (int i = 0; i < nenv; i++) {
        memcpy(w, env[i], strlen(env[i]) + 1);
        w += strlen(env[i]) + 1;
    }

    return block;
}

int proc_run(char *const *argv, const char *cwd, const char *log_path,
             char *const *env, int nenv, PROC_RESULT *out) {
    out->exit_code = -1;
    out->signaled  = 0;
    out->signal    = 0;

    size_t cap = 4096;
    for (int i = 0; argv[i]; i++)
        cap += strlen(argv[i]) * 2 + 4;

    char *cmdline = (char *)malloc(cap);
    if (!cmdline) return -1;

    cmdline[0] = 0;

    size_t len = 0;

    for (int i = 0; argv[i]; i++) {
        if (i) append_arg(cmdline, cap, &len, " ");

        append_arg(cmdline, cap, &len, argv[i]);
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    HANDLE hlog = CreateFileA(log_path, GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hlog == INVALID_HANDLE_VALUE) {
        free(cmdline);
        return -1;
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));

    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hlog;
    si.hStdError  = hlog;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    char *block = env_merge(env, nenv);

    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                             CREATE_UNICODE_ENVIRONMENT, block, cwd, &si, &pi);

    free(block);

    free(cmdline);
    CloseHandle(hlog);

    if (!ok) return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    out->exit_code = (int)code;
    return 0;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

int proc_run(char *const *argv, const char *cwd, const char *log_path,
             char *const *env, int nenv, PROC_RESULT *out) {
    out->exit_code = -1;
    out->signaled  = 0;
    out->signal    = 0;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        for (int i = 0; i < nenv; i++)
            putenv(env[i]);

        if (cwd && chdir(cwd) != 0) _exit(127);

        int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) _exit(127);

        if (dup2(fd, 1) < 0) _exit(127);
        if (dup2(fd, 2) < 0) _exit(127);
        if (fd > 2) close(fd);

        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;

    for (;;) {
        if (waitpid(pid, &status, 0) >= 0) break;
        if (errno != EINTR) return -1;
    }

    if (WIFEXITED(status)) {
        out->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out->exit_code = -1;
        out->signaled  = 1;
        out->signal    = WTERMSIG(status);
    }

    return 0;
}

#endif
