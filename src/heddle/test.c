#include "test.h"

#include "emit.h"
#include "proc.h"
#include "sys.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define TEST_OUT ".heddle\\test.out"
#else
#define TEST_OUT ".heddle/test.out"
#endif

static int load(const HEDDLE_OPTS *o, PROJECT *p, char *err, size_t errsz) {
    if (sys_chdir(o->root) != 0) {
        snprintf(err, errsz, "cannot enter project directory '%s'", o->root);
        return -1;
    }

    project_set_target(o->target);

    if (project_load(p, ".", o->toolchain, err, errsz) != 0) return -1;

    if (project_check(p, err, errsz) != 0) {
        project_free(p);
        return -1;
    }

    return 0;
}

static TEST_CASE *case_add(TEST_SET *s, const char *name) {
    if (s->n == s->cap) {
        int        newcap = s->cap ? s->cap * 2 : 8;
        TEST_CASE *items  = (TEST_CASE *)realloc(s->items, sizeof(TEST_CASE) * (size_t)newcap);

        if (!items) return NULL;

        s->items = items;
        s->cap   = newcap;
    }

    TEST_CASE *c = &s->items[s->n++];
    memset(c, 0, sizeof(*c));

    c->name   = sys_dup(name);
    c->expect = 0;

    return c;
}

void test_free(TEST_SET *s) {
    for (int i = 0; i < s->n; i++) {
        free(s->items[i].name);
        free(s->items[i].stdout_match);

        for (int k = 0; k < s->items[i].nargs; k++) free(s->items[i].args[k]);

        free(s->items[i].args);
    }

    free(s->items);
    memset(s, 0, sizeof(*s));
}

int test_load(TEST_SET *s, const PROJECT *p, char *err, size_t errsz) {
    char cfg[4096];
    snprintf(cfg, sizeof(cfg), "%s/heddle.toml", p->root);

    TOML t;
    toml_init(&t);

    if (toml_parse(&t, cfg, err, errsz) != 0) {
        toml_free(&t);
        return -1;
    }

    for (int i = 0; i < p->ntargets; i++) {
        const TARGET *tg = &p->targets[i];
        char          sect[1024];

        snprintf(sect, sizeof(sect), "target.%s.test", tg->name);

        const char *stdo = toml_str(&t, sect, "stdout");
        const char *code = toml_str(&t, sect, "expect");
        int         has  = stdo != NULL || code != NULL || toml_arr(&t, sect, "args", 0) != NULL;

        if (!has) continue;

        TEST_CASE *c = case_add(s, tg->name);

        if (!c) {
            snprintf(err, errsz, "out of memory");
            toml_free(&t);
            return -1;
        }

        c->expect       = code ? atoi(code) : 0;
        c->stdout_match = stdo ? sys_dup(stdo) : NULL;

        for (int k = 0;; k++) {
            const char *a = toml_arr(&t, sect, "args", k);
            if (!a) break;

            char **next = (char **)realloc(c->args, sizeof(char *) * (size_t)(c->nargs + 1));
            if (!next) {
                snprintf(err, errsz, "out of memory");
                toml_free(&t);
                return -1;
            }

            c->args           = next;
            c->args[c->nargs] = sys_dup(a);
            c->nargs++;
        }
    }

    toml_free(&t);
    return 0;
}

static void resolve_bin(char *bin, size_t cap) {
#if defined(_WIN32)
    SYS_STAT st;

    if (sys_stat(bin, &st) == 0) return;

    size_t l = strlen(bin);

    if (l < 4 || strcmp(bin + l - 4, ".exe")) {
        if (l + 4 < cap) memcpy(bin + l, ".exe", 5);
    }
#else
    (void)bin;
    (void)cap;
#endif
}

static int run_case(const TEST_CASE *c, const char *bin, int verbose, char *out, size_t outcap) {
    char **argv = (char **)calloc((size_t)c->nargs + 2, sizeof(char *));

    if (!argv) return -1;

    argv[0] = (char *)bin;

    for (int i = 0; i < c->nargs; i++) argv[i + 1] = c->args[i];

    if (verbose) {
        printf("heddle:");
        for (int i = 0; argv[i]; i++) printf(" %s", argv[i]);
        printf("\n");
    }

    PROC_RESULT r;
    int         rc = proc_run(argv, NULL, TEST_OUT, NULL, 0, &r);

    free(argv);

    if (rc != 0) return -1;

    out[0] = 0;

    FILE *f = fopen(TEST_OUT, "rb");

    if (f) {
        size_t got = fread(out, 1, outcap - 1, f);
        out[got]   = 0;
        fclose(f);
    }

    return r.signaled ? -1 : r.exit_code;
}

int heddle_test(const HEDDLE_OPTS *o) {
    char    err[512] = {0};
    PROJECT p;

    if (load(o, &p, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    TEST_SET set;
    memset(&set, 0, sizeof(set));

    if (test_load(&set, &p, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        project_free(&p);
        return 1;
    }

    if (o->target && !project_target((PROJECT *)&p, o->target)) {
        fprintf(stderr, "heddle: unknown target '%s'\n", o->target);
        test_free(&set);
        project_free(&p);
        return 1;
    }

    if (set.n == 0) {
        fprintf(stderr, "heddle: no tests declared "
                        "(add [target.NAME.test] to heddle.toml)\n");
        test_free(&set);
        project_free(&p);
        return 1;
    }

    int pass = 0;
    int fail = 0;

    for (int i = 0; i < set.n; i++) {
        const TEST_CASE *c = &set.items[i];

        if (o->target && strcmp(o->target, c->name)) continue;

        if (!project_target((PROJECT *)&p, c->name)) {
            fprintf(stderr, "heddle: unknown target '%s'\n", c->name);
            fail++;
            continue;
        }

        HEDDLE_OPTS bo = *o;
        bo.target      = c->name;

        printf("[%s] build %s ... ", c->name, c->name);
        fflush(stdout);

        if (heddle_run(&bo) != 0) {
            printf("FAIL (build)\n");
            fail++;
            continue;
        }

        char *bin = emit_artifact(&p, project_target(&p, c->name));

        if (!bin) {
            printf("FAIL (no artifact)\n");
            fail++;
            continue;
        }

        char norm[4096];
        snprintf(norm, sizeof(norm), "%s", bin);
        resolve_bin(norm, sizeof(norm));

        char stdout_buf[8192];
        int  rc = run_case(c, norm, o->verbose, stdout_buf, sizeof(stdout_buf));

        free(bin);

        int ok = rc == c->expect;

        if (ok && c->stdout_match && !strstr(stdout_buf, c->stdout_match)) ok = 0;

        if (ok) {
            printf("ok (exit %d)\n", rc);
            pass++;
        } else {
            printf("FAIL (exit %d, want %d)\n", rc, c->expect);

            if (c->stdout_match && !strstr(stdout_buf, c->stdout_match)) {
                printf("      stdout does not contain '%s'\n", c->stdout_match);

                if (stdout_buf[0]) printf("      got: %s\n", stdout_buf);
            }

            fail++;
        }
    }

    printf("heddle: %d passed, %d failed\n", pass, fail);

    test_free(&set);
    project_free(&p);
    return fail ? 1 : 0;
}
