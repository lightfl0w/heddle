#ifndef HEDDLE_INSTALL_H
#define HEDDLE_INSTALL_H

#include <stddef.h>

#include "project.h"

typedef struct {
    char *name;
    char **bin;
    int    nbin;
    char **lib;
    int    nlib;
    char **include;
    int    ninc;
    char **share;
    int    nshare;
    char **etc;
    int    netc;
    char **sysroot_lib;
    int    nsrlib;
    char **sysroot_include;
    int    nsrinc;
    char  *rootfs;
} INSTALL_RULE;

typedef struct {
    INSTALL_RULE *rules;
    int           n;
    int           cap;
} INSTALL_SET;

typedef struct {
    char *src;
    char *dst;
} INSTALL_ITEM;

typedef struct {
    INSTALL_ITEM *items;
    int           n;
    int           cap;
} INSTALL_PLAN;

int  install_load(INSTALL_SET *s, const PROJECT *p, char *err, size_t errsz);
INSTALL_RULE *install_add(INSTALL_SET *s, const char *name);
void install_free(INSTALL_SET *s);

INSTALL_RULE *install_rule(INSTALL_SET *s, const char *target);

int  install_has_any(const INSTALL_SET *s);

int  install_plan(INSTALL_SET *s, PROJECT *p, const char *target,
                  const char *prefix, const char *destdir,
                  INSTALL_PLAN *out, char *err, size_t errsz);

int  install_run(INSTALL_PLAN *pl, const char *log,
                 int verbose, char *err, size_t errsz);

int  install_write_log(const char *log, const INSTALL_PLAN *pl,
                       char *err, size_t errsz);

void install_log_path(char *buf, size_t cap, const char *root,
                      const char *destdir, const char *prefix);

int  install_dry_run(const INSTALL_PLAN *pl);

int  install_uninstall(const char *log, int verbose, char *err, size_t errsz);

void install_plan_free(INSTALL_PLAN *pl);

#endif
