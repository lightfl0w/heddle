#include "json.h"

#include "sys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void json_init(JSON *j) {
    memset(j, 0, sizeof(*j));
}

void json_free(JSON *j) {
    for (int i = 0; i < j->n; i++) {
        free(j->items[i].key);
        free(j->items[i].val);
    }

    free(j->items);
    memset(j, 0, sizeof(*j));
}

static void put(JSON *j, const char *key, const char *val) {
    if (j->n == j->cap) {
        int newcap = j->cap ? j->cap * 2 : 16;

        JSON_KV *items = (JSON_KV *)realloc(
            j->items, sizeof(JSON_KV) * (size_t)newcap);
        if (!items) return;

        j->items = items;
        j->cap   = newcap;
    }

    j->items[j->n].key = sys_dup(key);
    j->items[j->n].val = sys_dup(val);
    j->n++;
}

typedef struct {
    const char *p;
    const char *end;
    JSON       *j;
    char       *err;
    size_t      errsz;
    int         depth;
} PARSER;

static void skip_ws(PARSER *s) {
    while (s->p < s->end && isspace((unsigned char)*s->p)) s->p++;
}

static char *read_string(PARSER *s) {
    if (s->p >= s->end || *s->p != '"') return NULL;

    s->p++;

    size_t cap = 64, n = 0;
    char  *out = (char *)malloc(cap);

    if (!out) return NULL;

    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;

        if (c == '\\' && s->p < s->end) {
            char e = *s->p++;

            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'r') c = '\r';
            else if (e == 'b') c = '\b';
            else if (e == 'f') c = '\f';
            else c = e;
        }

        if (n + 1 >= cap) {
            cap *= 2;
            char *big = (char *)realloc(out, cap);
            if (!big) { free(out); return NULL; }
            out = big;
        }

        out[n++] = c;
    }

    if (s->p < s->end) s->p++;

    out[n] = 0;
    return out;
}

static char *read_primitive(PARSER *s) {
    const char *b = s->p;

    while (s->p < s->end && !strchr(",}] \t\r\n", *s->p)) s->p++;

    size_t n = (size_t)(s->p - b);
    char  *out = (char *)malloc(n + 1);

    if (!out) return NULL;

    memcpy(out, b, n);
    out[n] = 0;
    return out;
}

static void parse_value(PARSER *s, const char *path);

static void parse_object(PARSER *s, const char *path) {
    skip_ws(s);

    if (s->p >= s->end || *s->p != '{') return;

    s->p++;

    for (;;) {
        skip_ws(s);

        if (s->p >= s->end) return;
        if (*s->p == '}') { s->p++; return; }

        char *key = read_string(s);

        if (!key) return;

        skip_ws(s);

        if (s->p < s->end && *s->p == ':') s->p++;

        char sub[1024];

        if (path[0])
            snprintf(sub, sizeof(sub), "%s.%s", path, key);
        else
            snprintf(sub, sizeof(sub), "%s", key);

        free(key);

        parse_value(s, sub);

        skip_ws(s);

        if (s->p < s->end && *s->p == ',') { s->p++; continue; }
        if (s->p < s->end && *s->p == '}') { s->p++; return; }
    }
}

static void parse_array(PARSER *s, const char *path) {
    skip_ws(s);

    if (s->p >= s->end || *s->p != '[') return;

    s->p++;

    int idx = 0;

    for (;;) {
        skip_ws(s);

        if (s->p >= s->end) return;
        if (*s->p == ']') { s->p++; return; }

        char sub[1024];
        snprintf(sub, sizeof(sub), "%s[%d]", path, idx++);

        parse_value(s, sub);

        skip_ws(s);

        if (s->p < s->end && *s->p == ',') { s->p++; continue; }
        if (s->p < s->end && *s->p == ']') { s->p++; return; }
    }
}

static void parse_value(PARSER *s, const char *path) {
    skip_ws(s);

    if (s->p >= s->end) return;

    if (s->depth++ > 64) {
        if (s->err) snprintf(s->err, s->errsz, "%s: json nested too deep",
                             path);
        s->depth--;
        return;
    }

    if (*s->p == '{') {
        parse_object(s, path);
    } else if (*s->p == '[') {
        parse_array(s, path);
    } else if (*s->p == '"') {
        char *v = read_string(s);

        if (v) { put(s->j, path, v); free(v); }
    } else {
        char *v = read_primitive(s);

        if (v) { put(s->j, path, v); free(v); }
    }

    s->depth--;
}

static int json_parse_text(JSON *j, const char *text, const char *src,
                           char *err, size_t errsz) {
    PARSER s;
    memset(&s, 0, sizeof(s));

    s.p     = text;
    s.end   = text + strlen(text);
    s.j     = j;
    s.err   = err;
    s.errsz = errsz;

    skip_ws(&s);

    if (s.p >= s.end || *s.p != '{') {
        snprintf(err, errsz, "%s: not a json object", src ? src : "<json>");
        return -1;
    }

    parse_value(&s, "");

    skip_ws(&s);

    if (s.p < s.end) {
        snprintf(err, errsz, "%s: trailing data in json", src ? src : "<json>");
        return -1;
    }

    return 0;
}

int json_parse(JSON *j, const char *path, char *err, size_t errsz) {
    FILE *f = fopen(path, "rb");

    if (!f) {
        snprintf(err, errsz, "cannot read %s", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)n + 1);

    if (!buf) {
        fclose(f);
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);

    int rc = json_parse_text(j, buf, path, err, errsz);

    free(buf);
    return rc;
}

const char *json_get(const JSON *j, const char *key) {
    for (int i = 0; i < j->n; i++)
        if (!strcmp(j->items[i].key, key)) return j->items[i].val;

    return NULL;
}

