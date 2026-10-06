#include "vcpkg.h"

#include "json.h"
#include "pkg.h"
#include "sys.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *join(const char *a, const char *b) {
    if (!a || !a[0] || !strcmp(a, ".")) return sys_dup(b);

    size_t n = strlen(a) + strlen(b) + 2;
    char  *p = (char *)malloc(n);

    if (p) snprintf(p, n, "%s/%s", a, b);

    return p;
}

static int exists(const char *p) {
    SYS_STAT st;
    return sys_stat(p, &st) == 0;
}

static void push(char ***v, int *n, const char *s) {
    char **next = (char **)realloc(*v, sizeof(char *) * (size_t)(*n + 1));
    if (!next) return;

    *v = next;
    (*v)[*n] = sys_dup(s);

    if ((*v)[*n]) (*n)++;
}

int vcpkg_port_load(VCPKG_PORT *v, const char *dir, char *err, size_t errsz) {
    memset(v, 0, sizeof(*v));

    v->dir = sys_dup(dir);

    char *path = join(dir, "vcpkg.json");

    if (!path || !exists(path)) {
        snprintf(err, errsz, "no vcpkg.json in %s", dir ? dir : ".");
        free(path);
        return -1;
    }

    JSON j;
    json_init(&j);

    if (json_parse(&j, path, err, errsz) != 0) {
        json_free(&j);
        free(path);
        return -1;
    }

    free(path);

    const char *name = json_get(&j, "name");

    if (!name) {
        snprintf(err, errsz, "vcpkg.json has no name");
        json_free(&j);
        return -1;
    }

    v->name = sys_dup(name);
    v->version = sys_dup(json_get(&j, "version"));
    v->desc = sys_dup(json_get(&j, "description"));
    v->homepage = sys_dup(json_get(&j, "homepage"));

    for (int i = 0;; i++) {
        char key[256];

        snprintf(key, sizeof(key), "dependencies[%d]", i);

        const char *d = json_get(&j, key);

        if (d) {
            push(&v->deps, &v->ndeps, d);
            continue;
        }

        char sub[256];
        snprintf(sub, sizeof(sub), "dependencies[%d].name", i);

        const char *n = json_get(&j, sub);

        if (!n) break;

        push(&v->deps, &v->ndeps, n);
    }

    json_free(&j);
    return 0;
}

void vcpkg_port_free(VCPKG_PORT *v) {
    free(v->name);
    free(v->version);
    free(v->desc);
    free(v->homepage);

    for (int i = 0; i < v->ndeps; i++) free(v->deps[i]);

    free(v->deps);
    free(v->dir);
    memset(v, 0, sizeof(*v));
}

typedef struct {
    const char *triplet;
    const char *arch;
    const char *abi;
    const char *flt;
} TRIPLET_MAP;

static const TRIPLET_MAP g_triplets[] = {
    { "x64-linux",              "x86_64",      "",       ""     },
    { "x86-linux",              "i686",        "",       ""     },
    { "arm64-linux",            "aarch64",     "",       ""     },
    { "arm-linux",              "armv7a",      "eabihf", "hard" },
    { "x64-windows",            "x86_64",      "",       ""     },
    { "x86-windows",            "i686",        "",       ""     },
    { "x64-windows-static",     "x86_64",      "",       ""     },
    { "x64-windows-static-md",  "x86_64",      "",       ""     },
    { "x64-osx",                "x86_64",      "",       ""     },
    { "arm64-osx",              "aarch64",     "",       ""     },
    { "thumbv7m-none-eabi",     "armv7m",      "eabi",   "soft" },
    { "thumbv7em-none-eabihf",  "armv7em",     "eabihf", "hard" },
    { "arm-none-eabi",          "armv7m",      "eabi",   "soft" },
};

int vcpkg_triplet(const char *triplet, char *arch, size_t acap,
                  char *abi, size_t bcap, char *flt, size_t fcap) {
    for (size_t i = 0; i < sizeof(g_triplets) / sizeof(g_triplets[0]); i++) {
        if (strcmp(g_triplets[i].triplet, triplet)) continue;

        snprintf(arch, acap, "%s", g_triplets[i].arch);
        snprintf(abi, bcap, "%s", g_triplets[i].abi);
        snprintf(flt, fcap, "%s", g_triplets[i].flt);
        return 0;
    }

    return -1;
}

static const char *const g_payload[] = { "include", "lib", "bin", "share" };

static const char *stem(const char *file, char *buf, size_t cap) {
    const char *b = strrchr(file, '/');

    b = b ? b + 1 : file;

    if (strncmp(b, "lib", 3)) return NULL;

    const char *dot = strrchr(b, '.');

    if (!dot || dot <= b + 3) return NULL;

    size_t n = (size_t)(dot - (b + 3));

    if (n + 1 > cap) return NULL;

    memcpy(buf, b + 3, n);
    buf[n] = 0;

    return buf;
}

static int write_libs(const char *libdir, const char *meta) {
    DIR *d = opendir(libdir);

    if (!d) return 0;

    FILE *f = fopen(meta, "w");

    if (!f) { closedir(d); return -1; }

    struct dirent *e;

    while ((e = readdir(d))) {
        char sb[256];

        if (!stem(e->d_name, sb, sizeof(sb))) continue;

        fprintf(f, "%s\n", sb);
    }

    fclose(f);
    closedir(d);
    return 0;
}

int vcpkg_import(const VCPKG_PORT *v, const char *installed,
                 const char *triplet, const char *store,
                 char *hash, size_t hcap, char *err, size_t errsz) {
    char *tree = join(installed, triplet);

    if (!tree || !sys_isdir(tree)) {
        snprintf(err, errsz, "no installed tree at %s",
                 tree ? tree : installed);
        free(tree);
        return -1;
    }

    char *dst = join(store, "library");
    char *d2  = dst ? join(dst, v->name) : NULL;
    char *d3  = d2 ? join(d2, v->version && v->version[0] ? v->version : "0") : NULL;

    free(dst);
    free(d2);

    if (!d3) {
        free(tree);
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    sys_mkpath(d3);

    int copied = 0;

    for (size_t i = 0; i < sizeof(g_payload) / sizeof(g_payload[0]); i++) {
        char *src = join(tree, g_payload[i]);

        if (src && sys_isdir(src)) {
            char *sub = join(d3, g_payload[i]);

            if (sub) {
                if (pkg_copy_tree(src, sub) == 0) copied++;
                free(sub);
            }
        }

        free(src);
    }

    free(tree);

    if (!copied) {
        snprintf(err, errsz,
                 "installed tree %s has no include/lib/bin/share", triplet);
        free(d3);
        return -1;
    }

    char *meta = join(d3, ".heddle-pkg");

    if (meta) {
        char *lib = join(d3, "lib");

        if (lib) {
            write_libs(lib, meta);
            free(lib);
        }

        free(meta);
    }

    pkg_hash_tree(d3, hash, hcap);

    free(d3);
    return 0;
}

int vcpkg_is_registry(const char *dir) {
    char *p = join(dir, "ports");
    char *v = join(dir, "versions");

    int ok = p && v && sys_isdir(p) && sys_isdir(v);

    free(p);
    free(v);
    return ok;
}


static void usage(void) {
    printf("usage: heddle vcpkg <command> [options]\n"
           "\n"
           "  show <port-dir>                 read a vcpkg.json\n"
           "  triplet <name>                  print the [target] mapping\n"
           "  check <registry-dir>            test for a vcpkg registry\n"
           "  import <port-dir> [options]     import an installed tree\n"
           "\n"
           "import options:\n"
           "  --from DIR      installed tree (default <port>/.vcpkg)\n"
           "  --triplet NAME  e.g. x64-linux\n"
           "  --store DIR     heddle store (default .heddle/store)\n"
           "  --lock FILE     update a heddle.lock (default ./heddle.lock)\n");
}

static const char *opt(int argc, char **argv, const char *name) {
    size_t n = strlen(name);

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], name) && i + 1 < argc) return argv[i + 1];
        if (!strncmp(argv[i], name, n) && argv[i][n] == '=') return argv[i] + n + 1;
    }

    return NULL;
}

static int cmd_show(const char *dir) {
    VCPKG_PORT v;
    char       err[256] = {0};

    if (vcpkg_port_load(&v, dir, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    printf("name     %s\n", v.name);
    printf("version  %s\n", v.version ? v.version : "-");

    if (v.desc)     printf("desc     %s\n", v.desc);
    if (v.homepage) printf("home     %s\n", v.homepage);

    for (int i = 0; i < v.ndeps; i++)
        printf("dep      %s\n", v.deps[i]);

    vcpkg_port_free(&v);
    return 0;
}

static int cmd_triplet(const char *name) {
    char arch[64], abi[64], flt[64];

    if (vcpkg_triplet(name, arch, sizeof(arch), abi, sizeof(abi),
                      flt, sizeof(flt)) != 0) {
        fprintf(stderr, "heddle: unknown triplet '%s'\n", name);
        return 1;
    }

    printf("[target]\narch = \"%s\"\n", arch);

    if (abi[0]) printf("abi = \"%s\"\n", abi);
    if (flt[0]) printf("float = \"%s\"\n", flt);

    return 0;
}

static int cmd_check(const char *dir) {
    if (!vcpkg_is_registry(dir)) {
        fprintf(stderr, "heddle: %s is not a vcpkg registry\n",
                dir ? dir : ".");
        return 1;
    }

    printf("heddle: %s looks like a vcpkg registry\n", dir);

    return 0;
}

static int cmd_import(int argc, char **argv) {
    const char *port      = NULL;
    const char *from      = opt(argc, argv, "--from");
    const char *triplet   = opt(argc, argv, "--triplet");
    const char *store     = opt(argc, argv, "--store");
    const char *lock      = opt(argc, argv, "--lock");

    for (int i = 3; i < argc; i++) {
        if (!strncmp(argv[i], "--", 2)) {
            if (!strchr(argv[i], '=')) i++;
            continue;
        }

        if (argv[i][0] == '-') continue;

        if (!port) port = argv[i];
    }

    if (!port) {
        fprintf(stderr, "heddle: import needs a port directory\n");
        return 2;
    }

    VCPKG_PORT v;
    char       err[256] = {0};

    if (vcpkg_port_load(&v, port, err, sizeof(err)) != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        return 1;
    }

    if (!triplet) {
        fprintf(stderr, "heddle: import needs --triplet\n");
        vcpkg_port_free(&v);
        return 2;
    }

    char *def_from = NULL;

    if (!from) {
        def_from = join(port, ".vcpkg");
        from     = def_from;
    }

    if (!store) store = ".heddle/store";

    char hash[64] = {0};

    int rc = vcpkg_import(&v, from, triplet, store, hash, sizeof(hash),
                          err, sizeof(err));

    free(def_from);

    if (rc != 0) {
        fprintf(stderr, "heddle: %s\n", err);
        vcpkg_port_free(&v);
        return 1;
    }

    printf("heddle: imported %s %s (%s)\n", v.name,
           v.version ? v.version : "-", triplet);
    printf("heddle: store  %s/library/%s/%s\n", store, v.name,
           v.version && v.version[0] ? v.version : "0");
    printf("heddle: sha256 %s\n", hash);

    char *def_lock = NULL;

    if (!lock) {
        SYS_STAT st;

        if (sys_stat("heddle.toml", &st) == 0) {
            def_lock = sys_dup("heddle.lock");
            lock     = def_lock;
        }
    }

    if (lock) {
        char lerr[256] = {0};

        if (pkg_lock_add(lock, "library", v.name,
                         v.version && v.version[0] ? v.version : "0",
                         hash, lerr, sizeof(lerr)) != 0) {
            fprintf(stderr, "heddle: %s\n", lerr);
            free(def_lock);
            vcpkg_port_free(&v);
            return 1;
        }

        printf("heddle: lock   %s\n", lock);
    }

    free(def_lock);
    vcpkg_port_free(&v);
    return 0;
}

int heddle_vcpkg(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }

    const char *cmd = argv[2];

    if (!strcmp(cmd, "show"))    return cmd_show(argc >= 4 ? argv[3] : ".");
    if (!strcmp(cmd, "triplet")) return cmd_triplet(argc >= 4 ? argv[3] : "");
    if (!strcmp(cmd, "check"))   return cmd_check(argc >= 4 ? argv[3] : ".");
    if (!strcmp(cmd, "import"))  return cmd_import(argc, argv);
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) { usage(); return 0; }

    fprintf(stderr, "heddle: unknown vcpkg command '%s'\n", cmd);
    usage();
    return 2;
}
