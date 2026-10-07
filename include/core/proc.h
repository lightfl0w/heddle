#ifndef PROC_H
#define PROC_H

typedef struct {
    int exit_code;
    int signaled;
    int signal;
} PROC_RESULT;

int proc_run(char *const *argv,
             const char *cwd,
             const char *log_path,
             char *const *env,
             int nenv,
             PROC_RESULT *out);

int proc_shell(const char *cmd,
               const char *cwd,
               const char *log_path,
               PROC_RESULT *out);

#endif
