
#include "toml.h"
#include "sys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void toml_init(TOML *t) {
    t->tables = NULL;
    t->count  = 0;
    t->cap    = 0;
}

static void table_free(TOML_TABLE *tab) {
    free(tab->name);

    for (int i = 0; i < tab->count; i++) {
        free(tab->items[i].key);
        free(tab->items[i].val);
    }

    free(tab->items);
}

void toml_free(TOML *t) {
    for (int i = 0; i < t->count; i++)
        table_free(&t->tables[i]);

    free(t->tables);
    toml_init(t);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;

    return s;
}

static TOML_TABLE *table_find(const TOML *t, const char *name) {
    for (int i = 0; i < t->count; i++)
        if (!strcmp(t->tables[i].name, name)) return &t->tables[i];

    return NULL;
}

static TOML_TABLE *table_add(TOML *t, const char *name,
                             char *err, size_t errsz) {
    TOML_TABLE *found = table_find(t, name);
    if (found) return found;

    if (t->count == t->cap) {
        int newcap = t->cap ? t->cap * 2 : 16;

        TOML_TABLE *tables = (TOML_TABLE *)realloc(
            t->tables, sizeof(TOML_TABLE) * (size_t)newcap);
        if (!tables) {
            snprintf(err, errsz, "out of memory");
            return NULL;
        }

        t->tables = tables;
        t->cap    = newcap;
    }

    TOML_TABLE *tab = &t->tables[t->count++];
    memset(tab, 0, sizeof(*tab));

    tab->name = sys_dup(name);
    if (!tab->name) {
        snprintf(err, errsz, "out of memory");
        return NULL;
    }

    return tab;
}

static int table_put(TOML_TABLE *tab, const char *key, const char *val,
                     char *err, size_t errsz) {
    for (int i = 0; i < tab->count; i++) {
        if (strcmp(tab->items[i].key, key)) continue;

        free(tab->items[i].val);
        tab->items[i].val = sys_dup(val);

        return tab->items[i].val ? 0 : -1;
    }

    if (tab->count == tab->cap) {
        int newcap = tab->cap ? tab->cap * 2 : 16;

        TOML_KV *items = (TOML_KV *)realloc(
            tab->items, sizeof(TOML_KV) * (size_t)newcap);
        if (!items) {
            snprintf(err, errsz, "out of memory");
            return -1;
        }

        tab->items = items;
        tab->cap   = newcap;
    }

    tab->items[tab->count].key = sys_dup(key);
    tab->items[tab->count].val = sys_dup(val);

    if (!tab->items[tab->count].key || !tab->items[tab->count].val)
        return -1;

    tab->count++;
    return 0;
}

static char *unquote(const char *s, size_t n) {
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }

    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s++;
        n -= 2;
    }

    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;

    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

int toml_parse(TOML *t, const char *path, char *err, size_t errsz) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errsz, "cannot read %s", path);
        return -1;
    }

    char line[8192];
    int  lineno = 0;

    TOML_TABLE *cur = table_add(t, "", err, errsz);
    if (!cur) {
        fclose(f);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        lineno++;

        char *s = trim(line);
        if (*s == 0 || *s == '#') continue;

        if (*s == '[') {
            char *close = strrchr(s, ']');
            if (!close) {
                snprintf(err, errsz, "line %d: missing ']'", lineno);
                fclose(f);
                return -1;
            }

            *close = 0;
            cur = table_add(t, trim(s + 1), err, errsz);

            if (!cur) {
                fclose(f);
                return -1;
            }

            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, errsz, "line %d: missing '='", lineno);
            fclose(f);
            return -1;
        }

        *eq = 0;

        char *key = trim(s);
        if (*key == 0) {
            snprintf(err, errsz, "line %d: empty key", lineno);
            fclose(f);
            return -1;
        }

        char *val = trim(eq + 1);

        if (*val == '[') {
            char *close = strrchr(val, ']');
            if (!close) {
                snprintf(err, errsz, "line %d: missing ']'", lineno);
                fclose(f);
                return -1;
            }

            *close = 0;

            int idx = 0;

            for (char *tok = strtok(val + 1, ","); tok; tok = strtok(NULL, ",")) {
                char *item = unquote(tok, strlen(tok));
                if (!item) {
                    snprintf(err, errsz, "out of memory");
                    fclose(f);
                    return -1;
                }

                char norm[1024];
                snprintf(norm, sizeof(norm), "%s[%d]", key, idx++);

                if (table_put(cur, norm, item, err, errsz) != 0) {
                    free(item);
                    fclose(f);
                    return -1;
                }

                free(item);
            }

            continue;
        }

        char *plain = unquote(val, strlen(val));
        if (!plain) {
            snprintf(err, errsz, "out of memory");
            fclose(f);
            return -1;
        }

        if (table_put(cur, key, plain, err, errsz) != 0) {
            free(plain);
            fclose(f);
            return -1;
        }

        free(plain);
    }

    fclose(f);
    return 0;
}

const char *toml_str(const TOML *t, const char *section, const char *key) {
    const TOML_TABLE *tab = table_find(t, section ? section : "");
    if (!tab) return NULL;

    for (int i = 0; i < tab->count; i++)
        if (!strcmp(tab->items[i].key, key)) return tab->items[i].val;

    return NULL;
}

const char *toml_arr(const TOML *t, const char *section, const char *key,
                     int index) {
    char norm[1024];
    snprintf(norm, sizeof(norm), "%s[%d]", key, index);

    return toml_str(t, section, norm);
}

int toml_int(const TOML *t, const char *section, const char *key,
             int fallback) {
    const char *v = toml_str(t, section, key);

    return v ? atoi(v) : fallback;
}

int toml_bool(const TOML *t, const char *section, const char *key,
              int fallback) {
    const char *v = toml_str(t, section, key);

    if (!v) return fallback;
    if (!strcmp(v, "true")) return 1;
    if (!strcmp(v, "false")) return 0;

    return fallback;
}

static int has_nested(const char *name) {
    return strchr(name, '.') != NULL;
}

int toml_sections(const TOML *t, const char *prefix, char ***out) {
    size_t n = strlen(prefix);
    int    k = 0;

    char **v = (char **)malloc(sizeof(char *));

    if (!v) return 0;

    for (int i = 0; i < t->count; i++) {
        const char *name = t->tables[i].name;

        if (strncmp(name, prefix, n) != 0) continue;
        if (has_nested(name + n)) continue;

        char **next = (char **)realloc(v, sizeof(char *) * (size_t)(k + 1));
        if (!next) break;

        v = next;

        v[k] = sys_dup(name + n);
        if (!v[k]) break;

        k++;
    }

    *out = v;
    return k;
}
