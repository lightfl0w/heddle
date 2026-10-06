#ifndef HEDDLE_STAR_H
#define HEDDLE_STAR_H

#include <stddef.h>

typedef enum {
    STAR_NONE,
    STAR_STR,
    STAR_NUM,
    STAR_BOOL,
    STAR_LIST,
    STAR_DICT,
    STAR_USERFN
} STAR_KIND;

typedef struct STAR STAR;

struct STAR {
    STAR_KIND kind;

    char  *s;
    double num;
    int    b;

    STAR **items;
    int    n;

    char **keys;
    STAR **vals;

    char **params;
    int    nparams;
    int    body_pos;
};

typedef struct STAR_TARGET {
    char *name;
    char *type;
    char **src;
    int    nsrc;
    char **inc;
    int    ninc;
    char **deps;
    int    ndeps;
    char **pkgdeps;
    int    npkgdeps;
    char  *platform;
    char **cflags;
    int    ncflags;
    char **ldflags;
    int    nldflags;
    char  *ldscript;
    char  *entry;
    char  *out;

    char **i_bin;
    int    n_i_bin;
    char **i_lib;
    int    n_i_lib;
    char **i_include;
    int    n_i_include;
    char **i_share;
    int    n_i_share;
    char **i_etc;
    int    n_i_etc;
    char  *i_rootfs;
} STAR_TARGET;

typedef struct STAR_PLATFORM {
    char *name;
    char *arch;
    char *abi;
    char *flt;
    char *sysroot;
    char *toolchain;
} STAR_PLATFORM;

typedef struct {
    char *root;
    char *file;

    char *project_name;
    char *build_dir;
    char *toolchain;
    char *store;

    char *arch, *abi, *flt, *sysroot;

    struct STAR_PLATFORM *pl;
    int                   npl;

    char **tc_name;
    char **tc_cc;
    char **tc_based;
    int    ntc;

    char **dep_name;
    char **dep_ver;
    char **dep_src;
    int    ndep;

    struct STAR_TARGET *tg;
    int                ntg;
} STAR_CFG;

void  star_init(void);

STAR *star_none(void);
STAR *star_str(const char *s);
STAR *star_num(double n);
STAR *star_bool(int b);
STAR *star_list(void);
STAR *star_dict(void);
STAR *star_dup(const STAR *v);
void  star_free(STAR *v);

void  star_list_add(STAR *l, STAR *v);
void  star_dict_put(STAR *d, const char *k, STAR *v);
STAR *star_userfn(char **params, int nparams, int body_pos);

STAR *star_get(const STAR *d, const char *key);
const char *star_as_str(const STAR *v);
double     star_as_num(const STAR *v, double fb);
int        star_len(const STAR *v);
STAR      *star_at(const STAR *v, int i);
void       star_add(STAR *a, const STAR *b);

int   star_run(const char *path, STAR_CFG *cfg, char *err, size_t errsz);

void  star_cfg_free(STAR_CFG *c);

STAR_TARGET *star_cfg_add_target(STAR_CFG *c, const char *name);

#endif
