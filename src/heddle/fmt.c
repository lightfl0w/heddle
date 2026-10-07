#include "fmt.h"

#include "glob.h"
#include "proc.h"
#include "project.h"
#include "sys.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define FMT_LOG ".heddle\\fmt.log"
#else
#define FMT_LOG ".heddle/fmt.log"
#endif

typedef struct {
    char **v;
    int    n;
    int    cap;
} FMT_LIST;

static void fl_add(FMT_LIST *l, const char *owned) {
    if (l->n == l->cap) {
        int    want = l->cap ? l->cap * 2 : 64;
        char **next = (char **)realloc(l->v, sizeof(char *) * (size_t)want);
        if (!next) return;

        l->v   = next;
        l->cap = want;
    }

    l->v[l->n++] = sys_dup(owned);
}

static void fl_free(FMT_LIST *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);

    free(l->v);
    memset(l, 0, sizeof(*l));
}

static int fl_has(const FMT_LIST *l, const char *path) {
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->v[i], path)) return 1;

    return 0;
}

static int is_header(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;

    return !strcmp(dot, ".h") || !strcmp(dot, ".hpp") || !strcmp(dot, ".hh") ||
           !strcmp(dot, ".hxx") || !strcmp(dot, ".inl");
}

static int fmt_lang(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;

    if (!strcmp(dot, ".c") || is_header(path)) return 1;

    return !strcmp(dot, ".cc") || !strcmp(dot, ".cpp") || !strcmp(dot, ".cxx");
}

static void add_file(FMT_LIST *l, const char *path) {
    SYS_STAT st;
    if (sys_stat(path, &st) != 0 || st.is_dir) return;
    if (fl_has(l, path)) return;

    fl_add(l, path);
}

static void add_headers_in(FMT_LIST *l, const char *dir) {
    static const char *const pats[] = {"*.h", "*.hpp", "*.hh", "*.hxx", "*.inl"};

    for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        GLOB_LIST gl;
        glob_init(&gl);
        if (glob_dir(dir, pats[i], &gl) == 0) {
            glob_free(&gl);
            continue;
        }

        for (int k = 0; k < gl.n; k++) {
            char full[4096];
            int  len = snprintf(full, sizeof(full), "%s/%s", dir, gl.items[k]);
            if (len <= 0 || (size_t)len >= sizeof(full)) continue;

            add_file(l, full);
        }

        glob_free(&gl);
    }
}

static void collect_target(const TARGET *t, FMT_LIST *out) {
    for (int k = 0; k < t->nsrc; k++)
        if (fmt_lang(t->src[k])) add_file(out, t->src[k]);

    for (int k = 0; k < t->ninc; k++) {
        SYS_STAT st;
        if (sys_stat(t->inc[k], &st) != 0) continue;

        if (st.is_dir) add_headers_in(out, t->inc[k]);
        else if (is_header(t->inc[k])) add_file(out, t->inc[k]);
    }
}

static void collect_project(const PROJECT *p, FMT_LIST *out) {
    for (int i = 0; i < p->ntargets; i++) collect_target(&p->targets[i], out);
}

static const char *const SKIP_DIRS[] = {".git",  ".heddle", ".xmake",       ".vscode",
                                        "build", "out",     "node_modules", NULL};

static int skip_dir(const char *name) {
    if (name[0] == '.') return 1;

    for (int i = 0; SKIP_DIRS[i]; i++)
        if (!strcmp(name, SKIP_DIRS[i])) return 1;

    return 0;
}

static void collect_walk(const char *dir, FMT_LIST *out, int depth) {
    if (depth > 12) return;

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char full[4096];
        int  len = snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (len <= 0 || (size_t)len >= sizeof(full)) continue;

        SYS_STAT st;
        if (sys_stat(full, &st) != 0) continue;

        if (st.is_dir) {
            if (!skip_dir(e->d_name)) collect_walk(full, out, depth + 1);
            continue;
        }

        if (fmt_lang(full)) add_file(out, full);
    }

    closedir(d);
}

static int has_config(const char *root) {
    static const char *const names[] = {".clang-format", "_clang-format"};
    char                     path[4096];

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, names[i]);
        SYS_STAT st;
        if (sys_stat(path, &st) == 0 && !st.is_dir) return 1;
    }

    return 0;
}

static const char *find_clang_format(void) {
    static char path[4096];

    const char *env  = getenv("PATH");
    char       *copy = env ? sys_dup(env) : NULL;
    char       *save = NULL;
    const char *hit  = NULL;

    const char *names[] = {"clang-format", "clang-format-" HEDDLE_FMT_CLANG_VER};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !hit; i++) {
        char probe[64];

        if (i == 0) snprintf(probe, sizeof(probe), "%s", names[i]);
#if defined(_WIN32)
        else snprintf(probe, sizeof(probe), "%s.exe", names[i]);
#else
        else snprintf(probe, sizeof(probe), "%s", names[i]);
#endif

        save = NULL;
        for (char *d = sys_tok(copy, ";:", &save); d && !hit; d = sys_tok(NULL, ";:", &save)) {
            if (!d[0]) continue;

            int len = snprintf(path, sizeof(path), "%s/%s", d, probe);
            if (len <= 0 || (size_t)len >= sizeof(path)) continue;

            SYS_STAT st;
            if (sys_stat(path, &st) == 0 && !st.is_dir) hit = path;
        }
    }

    free(copy);
    return hit;
}

int heddle_fmt(const HEDDLE_OPTS *o) {
    char err[512] = {0};

    if (sys_chdir(o->root) != 0) {
        fprintf(stderr, "heddle: cannot enter project directory '%s'\n", o->root);
        return 1;
    }

    if (!has_config(o->root)) {
        fprintf(stderr, "heddle: no .clang-format in %s\n", o->root);
        return 1;
    }

    const char *tool = find_clang_format();
    if (!tool) {
        fprintf(stderr, "heddle: clang-format not found on PATH\n");
        return 1;
    }

    PROJECT p;
    int     have_project = 0;
    memset(&p, 0, sizeof(p));

    SYS_STAT probe;
    if (sys_stat("heddle.star", &probe) == 0 || sys_stat("heddle.toml", &probe) == 0) {
        if (project_load(&p, ".", o->toolchain, err, sizeof(err)) != 0) {
            fprintf(stderr, "heddle: %s\n", err);
            return 1;
        }

        have_project = 1;

        if (o->target && !project_target(&p, o->target)) {
            fprintf(stderr, "heddle: unknown target '%s'\n", o->target);
            project_free(&p);
            return 1;
        }
    }

    FMT_LIST files = {0};
    if (!have_project) collect_walk(".", &files, 0);
    else if (o->target) collect_target(project_target(&p, o->target), &files);
    else collect_project(&p, &files);

    if (files.n == 0) {
        fprintf(stderr, "heddle: no C/C++ sources to format\n");
        if (have_project) project_free(&p);
        return 1;
    }

    sys_mkpath(".heddle");

    int bad     = 0;
    int changed = 0;

    for (int i = 0; i < files.n; i++) {
        SYS_STAT before;
        if (sys_stat(files.v[i], &before) != 0) continue;

        if (o->verbose) {
            printf("heddle: fmt %s\n", files.v[i]);
            fflush(stdout);
        }

        char *argv[] = {(char *)tool, (char *)"-i", files.v[i], NULL};

        PROC_RESULT r;
        if (proc_run(argv, NULL, FMT_LOG, NULL, 0, &r) != 0 || r.signaled || r.exit_code != 0) {
            fprintf(stderr, "heddle: clang-format failed on %s\n", files.v[i]);
            bad++;
            continue;
        }

        SYS_STAT after;
        if (sys_stat(files.v[i], &after) == 0 && after.mtime_ns != before.mtime_ns) {
            changed++;
            printf("  formatted %s\n", files.v[i]);
        }
    }

    remove(FMT_LOG);

    printf("heddle: %d file%s checked, %d reformatted\n", files.n, files.n == 1 ? "" : "s",
           changed);

    fl_free(&files);
    if (have_project) project_free(&p);

    return bad ? 1 : 0;
}
