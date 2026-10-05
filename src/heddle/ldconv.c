#include "ldconv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    char *origin;
    char *length;
} LD_REGION;

typedef struct {
    char      *entry;
    LD_REGION *regions;
    int        nreg;
    char     **places;
    int        nplace;
} LD_SCRIPT;

typedef struct {
    const char *p;
    const char *end;
} SCAN;

static void skip_ws(SCAN *s) {
    while (s->p < s->end && isspace((unsigned char)*s->p)) s->p++;
}

static void skip_comment(SCAN *s) {
    if (s->p + 1 < s->end && s->p[0] == '/' && s->p[1] == '*') {
        s->p += 2;
        while (s->p + 1 < s->end && !(s->p[0] == '*' && s->p[1] == '/')) s->p++;
        if (s->p + 1 < s->end) s->p += 2;
    }
}

static int ident(SCAN *s, char *buf, size_t cap) {
    skip_ws(s);
    size_t n = 0;

    while (s->p < s->end && (isalnum((unsigned char)*s->p) ||
                             *s->p == '_' || *s->p == '.' || *s->p == '-')) {
        if (n + 1 < cap) buf[n++] = *s->p;
        s->p++;
    }

    if (buf) buf[n] = 0;

    return n > 0;
}

static int number(SCAN *s, char *buf, size_t cap) {
    skip_ws(s);
    size_t n = 0;

    while (s->p < s->end && (isalnum((unsigned char)*s->p) || *s->p == 'x')) {
        if (n + 1 < cap) buf[n++] = *s->p;
        s->p++;
    }

    if (buf) buf[n] = 0;

    return n > 0;
}

static void push_region(LD_SCRIPT *L, const char *name,
                        const char *origin, const char *length) {
    LD_REGION *nr = (LD_REGION *)realloc(L->regions,
                                         sizeof(LD_REGION) * (size_t)(L->nreg + 1));
    if (!nr) return;

    L->regions = nr;
    L->regions[L->nreg].name   = name ? strdup(name) : NULL;
    L->regions[L->nreg].origin = origin ? strdup(origin) : NULL;
    L->regions[L->nreg].length = length ? strdup(length) : NULL;
    L->nreg++;
}

static void push_place(LD_SCRIPT *L, const char *region) {
    char **np = (char **)realloc(L->places, sizeof(char *) * (size_t)(L->nplace + 1));
    if (!np) return;

    L->places = np;
    L->places[L->nplace++] = region ? strdup(region) : NULL;
}

static void parse_memory(LD_SCRIPT *L, SCAN *s) {
    for (;;) {
        skip_ws(s);
        skip_comment(s);
        skip_ws(s);

        if (s->p >= s->end || *s->p == '}') break;

        char name[256], origin[64] = "", length[64] = "";

        if (!ident(s, name, sizeof(name))) break;

        skip_ws(s);
        if (s->p < s->end && *s->p == '(') {
            while (s->p < s->end && *s->p != ')') s->p++;
            if (s->p < s->end) s->p++;
        }

        skip_ws(s);
        if (s->p < s->end && *s->p == ':') s->p++;

        for (;;) {
            skip_ws(s);
            if (s->p >= s->end || *s->p == '}') break;

            char key[64];

            if (!ident(s, key, sizeof(key))) break;

            skip_ws(s);
            if (s->p < s->end && *s->p == '=') s->p++;

            char val[64];

            if (!number(s, val, sizeof(val))) break;

            if (!strcasecmp(key, "ORIGIN")) snprintf(origin, sizeof(origin), "%s", val);
            else if (!strcasecmp(key, "LENGTH")) snprintf(length, sizeof(length), "%s", val);

            skip_ws(s);
            if (s->p < s->end && *s->p == ',') { s->p++; continue; }
            break;
        }

        push_region(L, name, origin, length);
    }
}

static void parse_sections(LD_SCRIPT *L, SCAN *s) {
    for (;;) {
        skip_ws(s);
        skip_comment(s);
        skip_ws(s);

        if (s->p >= s->end || *s->p == '}') break;

        if (*s->p == '.') {
            s->p++;

            while (s->p < s->end && (isalnum((unsigned char)*s->p) || *s->p == '_' ||
                                     *s->p == '.')) s->p++;

            while (s->p < s->end && *s->p != '{' && *s->p != '>' && *s->p != ';')
                s->p++;

            if (s->p < s->end && *s->p == '{') {
                int depth = 0;

                while (s->p < s->end) {
                    if (*s->p == '{') depth++;
                    else if (*s->p == '}') { depth--; if (!depth) { s->p++; break; } }
                    s->p++;
                }
            }

            skip_ws(s);

            if (s->p < s->end && *s->p == '>') {
                s->p++;

                char reg[256];

                if (ident(s, reg, sizeof(reg))) push_place(L, reg);
            }

            while (s->p < s->end && *s->p != ';' && *s->p != '}') s->p++;
            if (s->p < s->end && *s->p == ';') s->p++;

            continue;
        }

        s->p++;
    }
}

static int ld_parse(LD_SCRIPT *L, const char *path, char *err, size_t errsz) {
    FILE *f = fopen(path, "rb");

    if (!f) {
        snprintf(err, errsz, "cannot read %s", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); snprintf(err, errsz, "out of memory"); return -1; }

    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);

    SCAN s = { buf, buf + got };

    for (;;) {
        char kw[64];

        skip_ws(&s);
        skip_comment(&s);

        if (s.p >= s.end) break;

        const char *b = s.p;

        if (!ident(&s, kw, sizeof(kw))) { s.p = b + 1; continue; }

        if (!strcasecmp(kw, "ENTRY")) {
            skip_ws(&s);
            if (s.p < s.end && *s.p == '(') {
                s.p++;
                char sym[256];
                if (ident(&s, sym, sizeof(sym))) {
                    free(L->entry);
                    L->entry = strdup(sym);
                }
            }
            continue;
        }

        if (!strcasecmp(kw, "MEMORY")) {
            skip_ws(&s);
            if (s.p < s.end && *s.p == '{') { s.p++; parse_memory(L, &s); }
            continue;
        }

        if (!strcasecmp(kw, "SECTIONS")) {
            skip_ws(&s);
            if (s.p < s.end && *s.p == '{') { s.p++; parse_sections(L, &s); }
            continue;
        }
    }

    free(buf);
    return 0;
}

static void ld_free(LD_SCRIPT *L) {
    for (int i = 0; i < L->nreg; i++) {
        free(L->regions[i].name);
        free(L->regions[i].origin);
        free(L->regions[i].length);
    }

    for (int i = 0; i < L->nplace; i++) free(L->places[i]);

    free(L->regions);
    free(L->places);
    free(L->entry);
    memset(L, 0, sizeof(*L));
}

static unsigned long long num(const char *s) {
    if (!s || !*s) return 0;

    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    unsigned long long mul = 1;

    if (end) {
        if (*end == 'K' || *end == 'k') mul = 1024ULL;
        else if (*end == 'M' || *end == 'm') mul = 1024ULL * 1024;
        else if (*end == 'G' || *end == 'g') mul = 1024ULL * 1024 * 1024;
    }

    return v * mul;
}

static const char *pick_region(LD_SCRIPT *L) {
    return L->nreg ? L->regions[0].name : NULL;
}

static int emit_icf(LD_SCRIPT *L, FILE *f, char *err, size_t errsz) {
    if (!L->nreg) {
        snprintf(err, errsz, "cannot convert: no MEMORY region in GNU script");
        return -1;
    }

    if (L->entry) fprintf(f, "define symbol __entry = %s;\n", L->entry);

    for (int i = 0; i < L->nreg; i++)
        fprintf(f, "define region %s = mem:[from 0x%llx to 0x%llx];\n",
                L->regions[i].name, num(L->regions[i].origin),
                num(L->regions[i].origin) + num(L->regions[i].length) - 1);

    if (L->nplace) {
        for (int i = 0; i < L->nplace; i++)
            if (L->places[i]) fprintf(f, "place in %s { readonly, readwrite };\n", L->places[i]);
    } else {
        fprintf(f, "place in %s { readonly, readwrite };\n", pick_region(L));
    }

    return 0;
}

static int emit_scatter(LD_SCRIPT *L, FILE *f, char *err, size_t errsz) {
    if (!L->nreg) {
        snprintf(err, errsz, "cannot convert: no MEMORY region in GNU script");
        return -1;
    }

    if (L->entry) fprintf(f, "ENTRY %s\n", L->entry);

    for (int i = 0; i < L->nreg; i++)
        fprintf(f, "  %s 0x%llx 0x%llx {\n    * (+RO, +RW, +ZI)\n  }\n",
                L->regions[i].name, num(L->regions[i].origin),
                num(L->regions[i].length));

    return 0;
}

int ldconv_convert(const char *gnu_ld, const char *family,
                   const char *out_path, char *err, size_t errsz) {
    LD_SCRIPT L;
    memset(&L, 0, sizeof(L));

    if (ld_parse(&L, gnu_ld, err, errsz) != 0) {
        ld_free(&L);
        return -1;
    }

    FILE *f = fopen(out_path, "wb");

    if (!f) {
        snprintf(err, errsz, "cannot write %s", out_path);
        ld_free(&L);
        return -1;
    }

    int rc;

    if (!strcmp(family, "iar")) {
        fprintf(f, "// generated from %s\n", gnu_ld);
        rc = emit_icf(&L, f, err, errsz);
    } else if (!strcmp(family, "armcc")) {
        fprintf(f, "; generated from %s\n", gnu_ld);
        rc = emit_scatter(&L, f, err, errsz);
    } else {
        snprintf(err, errsz, "no script converter for family '%s'", family);
        rc = -1;
    }

    fclose(f);
    ld_free(&L);

    if (rc != 0) remove(out_path);

    return rc;
}
