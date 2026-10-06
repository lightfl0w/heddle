#include "heddle.h"

#include "emit.h"
#include "init.h"
#include "vcpkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    HEDDLE_OPTS o;
    int         check_only;
    int         list_tools;
    int         exec_after;
    int         tool_install;
    int         tool_plan;
    int         env_verify;
    int         do_install;
    int         do_uninstall;
} ARGS;

static int usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [build] TARGET\n"
            "       %s run TARGET\n"
            "       %s init [TYPE] [DIR]   create a new project\n"
            "       %s check\n"
            "       %s toolchains\n"
            "       %s tool install        restore toolchain + dependencies\n"
            "       %s tool plan           print the resolved package plan\n"
            "       %s env verify          verify the environment matches the lock\n"
            "       %s install [TARGET]    install build products\n"
            "       %s uninstall           remove what install wrote\n"
            "       %s vcpkg <cmd>         vcpkg package compatibility\n"
            "\n"
            "options:\n"
            "  -C DIR            project root (default .)\n"
            "  -j N              parallel jobs\n"
            "  -t NAME           toolchain, default auto-detect\n"
            "  -v, --verbose     print toolchain and cache stats\n"
            "  --cache DIR       CAS root\n"
            "  --remote URL      remote CAS, dir or http(s)://\n"
            "  --no-cache        disable cache\n"
            "  --registry DIR|URL  package registry (default $HEDDLE_REGISTRY)\n"
            "  --offline         never contact the registry\n"
            "  --prefix DIR      install prefix (required for install)\n"
            "  --destdir DIR     stage under DIR, do not touch the system\n"
            "  --dry-run         print the install plan only\n",
            prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
    return 2;
}

static int take(const char *arg, const char *name, int argc, char **argv,
                int *i, const char **out) {
    size_t n = strlen(name);

    if (!strcmp(arg, name)) {
        if (*i + 1 >= argc) {
            fprintf(stderr, "heddle: %s needs a value\n", name);
            return -1;
        }

        *out = argv[++*i];
        return 1;
    }

    if (name[0] == '-' && name[1] == '-' && !strncmp(arg, name, n) &&
        arg[n] == '=' && arg[n + 1]) {
        *out = arg + n + 1;
        return 1;
    }

    if (name[1] != '-' && !strncmp(arg, name, n) && arg[n]) {
        *out = arg + n;
        return 1;
    }

    return 0;
}

static int parse(int argc, char **argv, ARGS *a) {
    HEDDLE_OPTS *o = &a->o;
    const char  *v = NULL;
    int          t = 0;

    int sub = 0;

    o->root = ".";
    o->registry = getenv("HEDDLE_REGISTRY");

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (sub == 1) {
            sub = 0;

            if (!strcmp(arg, "install")) { a->tool_install = 1; continue; }
            if (!strcmp(arg, "plan"))    { a->tool_plan    = 1; continue; }
            if (!strcmp(arg, "list"))    { a->tool_plan    = 1; continue; }

            fprintf(stderr, "heddle: unknown tool subcommand '%s'\n", arg);
            return 2;
        }

        if (sub == 2) {
            sub = 0;

            if (!strcmp(arg, "verify")) { a->env_verify = 1; continue; }

            fprintf(stderr, "heddle: unknown env subcommand '%s'\n", arg);
            return 2;
        }

        if (!strcmp(arg, "--no-cache"))       o->no_cache = 1;
        else if (!strcmp(arg, "--offline"))   o->offline = 1;
        else if (!strcmp(arg, "-v"))          o->verbose = 1;
        else if (!strcmp(arg, "--verbose"))    o->verbose = 1;
        else if (!strcmp(arg, "-h"))          return usage(argv[0]);
        else if (!strcmp(arg, "--help"))      return usage(argv[0]);

        else if (!strcmp(arg, "toolchains"))  a->list_tools = 1;
        else if (!strcmp(arg, "check"))       a->check_only = 1;
        else if (!strcmp(arg, "install"))     a->do_install = 1;
        else if (!strcmp(arg, "uninstall"))   a->do_uninstall = 1;
        else if (!strcmp(arg, "--dry-run"))   o->dry_run = 1;

        else if (!strcmp(arg, "tool"))        sub = 1;
        else if (!strcmp(arg, "env"))         sub = 2;

        else if (!strcmp(arg, "build"))       continue;
        else if (!strcmp(arg, "run"))         a->exec_after = 1;

        else if ((t = take(arg, "-C", argc, argv, &i, &v)) < 0) return 2;
        else if (t) o->root = v;

        else if ((t = take(arg, "-j", argc, argv, &i, &v)) < 0) return 2;
        else if (t) o->jobs = atoi(v);

        else if ((t = take(arg, "-t", argc, argv, &i, &v)) < 0) return 2;
        else if (t) o->toolchain = v;

        else if ((t = take(arg, "--cache", argc, argv, &i, &v)) < 0) return 2;
        else if (t) o->cache = v;

        else if ((t = take(arg, "--remote", argc, argv, &i, &v)) < 0) return 2;
        else if (t) o->remote = v;

        else if ((t = take(arg, "--registry", argc, argv, &i, &v)) < 0) return 2;
        else if (t) o->registry = v;

        else if ((t = take(arg, "--prefix", argc, argv, &i, &v)) < 0) return 2;
        else if (t) o->prefix = v;

        else if ((t = take(arg, "--destdir", argc, argv, &i, &v)) < 0) return 2;
        else if (t) o->destdir = v;

        else if (arg[0] == '-') {
            fprintf(stderr, "heddle: unknown option '%s'\n", arg);
            return 2;
        } else if (!o->target) {
            o->target = arg;
        } else {
            fprintf(stderr, "heddle: unexpected '%s'\n", arg);
            return 2;
        }
    }

    if (sub) {
        fprintf(stderr, "heddle: incomplete command\n");
        return 2;
    }

    if (a->list_tools || a->check_only || a->tool_install || a->tool_plan ||
        a->env_verify || a->do_install || a->do_uninstall)
        return 0;

    if (!o->target) return usage(argv[0]);

    return 0;
}

int main(int argc, char **argv) {
    ARGS a;

    if (argc >= 3 && !strcmp(argv[1], "ldconv"))
        return heddle_ldconv(argc, argv);

    if (argc >= 2 && !strcmp(argv[1], "init"))
        return heddle_init(argc, argv);

    if (argc >= 2 && !strcmp(argv[1], "vcpkg"))
        return heddle_vcpkg(argc, argv);

    emit_set_self(argv[0]);

    memset(&a, 0, sizeof(a));

    int rc = parse(argc, argv, &a);

    if (rc) return rc;
    if (a.list_tools)   return heddle_toolchains();
    if (a.check_only)   return heddle_check(&a.o);
    if (a.do_install)   return heddle_install(&a.o);
    if (a.do_uninstall) return heddle_uninstall(&a.o);
    if (a.tool_install) return heddle_tool_install(&a.o);
    if (a.tool_plan)    return heddle_tool_plan(&a.o);
    if (a.env_verify)   return heddle_env_verify(&a.o);
    if (a.exec_after)   return heddle_exec(&a.o);

    return heddle_run(&a.o);
}
