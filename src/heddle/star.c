#include "star.h"

#include "glob.h"
#include "sys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static STAR g_none;

void star_init(void) {
    memset(&g_none, 0, sizeof(g_none));
    g_none.kind = STAR_NONE;
}

STAR *star_none(void) {
    return &g_none;
}

static STAR *alloc(STAR_KIND k) {
    STAR *v = (STAR *)calloc(1, sizeof(STAR));
    if (v) v->kind = k;

    return v;
}

STAR *star_str(const char *s) {
    STAR *v = alloc(STAR_STR);
    if (v) v->s = sys_dup(s ? s : "");

    return v;
}

STAR *star_num(double n) {
    STAR *v = alloc(STAR_NUM);
    if (v) v->num = n;

    return v;
}

STAR *star_bool(int b) {
    STAR *v = alloc(STAR_BOOL);
    if (v) v->b = b ? 1 : 0;

    return v;
}

STAR *star_list(void) {
    return alloc(STAR_LIST);
}
STAR *star_dict(void) {
    return alloc(STAR_DICT);
}

void star_free(STAR *v) {
    if (!v || v == &g_none) return;

    free(v->s);
    if (v->kind == STAR_LIST)
        for (int i = 0; i < v->n; i++) star_free(v->items[i]);

    if (v->kind == STAR_DICT)
        for (int i = 0; i < v->n; i++) {
            free(v->keys[i]);
            star_free(v->vals[i]);
        }

    for (int i = 0; i < v->nparams; i++) free(v->params[i]);

    free(v->params);
    free(v->items);
    free(v->keys);
    free(v->vals);
    free(v);
}

STAR *star_dup(const STAR *v) {
    if (!v) return star_none();

    STAR *d = alloc(v->kind);
    if (!d) return star_none();

    d->s   = v->s;
    d->num = v->num;
    d->b   = v->b;
    if (v->kind == STAR_STR) {
        d->s = sys_dup(v->s);
    } else if (v->kind == STAR_LIST) {
        for (int i = 0; i < v->n; i++) star_list_add(d, star_dup(v->items[i]));
    } else if (v->kind == STAR_DICT) {
        for (int i = 0; i < v->n; i++) star_dict_put(d, v->keys[i], star_dup(v->vals[i]));
    }

    return d;
}

void star_list_add(STAR *l, STAR *v) {
    STAR **next = (STAR **)realloc(l->items, sizeof(STAR *) * (size_t)(l->n + 1));
    if (!next) {
        star_free(v);
        return;
    }

    l->items         = next;
    l->items[l->n++] = v;
}

void star_dict_put(STAR *d, const char *k, STAR *v) {
    for (int i = 0; i < d->n; i++) {
        if (strcmp(d->keys[i], k)) continue;

        star_free(d->vals[i]);
        d->vals[i] = v;
        return;
    }

    STAR **nv = (STAR **)realloc(d->vals, sizeof(STAR *) * (size_t)(d->n + 1));
    char **nk = (char **)realloc(d->keys, sizeof(char *) * (size_t)(d->n + 1));
    if (!nv || !nk) {
        star_free(v);
        return;
    }

    d->vals       = nv;
    d->keys       = nk;
    d->keys[d->n] = sys_dup(k);
    d->vals[d->n] = v;
    d->n++;
}

STAR *star_get(const STAR *d, const char *key) {
    if (!d || d->kind != STAR_DICT) return NULL;

    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->keys[i], key)) return d->vals[i];

    return NULL;
}

const char *star_as_str(const STAR *v) {
    if (!v) return NULL;

    return v->kind == STAR_STR ? v->s : NULL;
}

double star_as_num(const STAR *v, double fb) {
    if (!v) return fb;
    if (v->kind == STAR_NUM) return v->num;
    if (v->kind == STAR_BOOL) return v->b;

    return fb;
}

int star_len(const STAR *v) {
    return v ? v->n : 0;
}

STAR *star_at(const STAR *v, int i) {
    if (!v || i < 0 || i >= v->n) return NULL;

    return v->items[i];
}

static int is_int(double n) {
    return n == (double)(long long)n;
}

static char *num_str(double n) {
    char buf[64];
    if (is_int(n)) snprintf(buf, sizeof(buf), "%.0f", n);
    else snprintf(buf, sizeof(buf), "%g", n);

    return sys_dup(buf);
}

static void append(char **dst, const char *tail) {
    char  *s = *dst ? *dst : sys_dup("");
    size_t n = strlen(s) + strlen(tail) + 1;
    char  *r = (char *)malloc(n);
    if (r) snprintf(r, n, "%s%s", s, tail);

    free(s);
    *dst = r ? r : sys_dup("");
}

static char *as_text(const STAR *v) {
    if (v->kind == STAR_STR) return sys_dup(v->s);
    if (v->kind == STAR_BOOL) return sys_dup(v->b ? "True" : "False");

    return num_str(v->num);
}

void star_add(STAR *a, const STAR *b) {
    if (!a || !b) return;

    if (a->kind == STAR_NUM && b->kind == STAR_NUM) {
        a->num += b->num;
    } else if (a->kind == STAR_LIST && b->kind == STAR_LIST) {
        for (int i = 0; i < b->n; i++) star_list_add(a, star_dup(b->items[i]));
    } else if (a->kind == STAR_STR || a->kind == STAR_NUM || a->kind == STAR_BOOL) {
        char *l = as_text(a);
        char *r = as_text(b);
        free(a->s);
        a->s    = sys_dup("");
        a->kind = STAR_STR;
        append(&a->s, l);
        append(&a->s, r);
        free(l);
        free(r);
    }
}

typedef enum {
    T_EOF,
    T_NEWLINE,
    T_INDENT,
    T_DEDENT,
    T_NAME,
    T_NUM,
    T_STR,
    T_LP,
    T_RP,
    T_LB,
    T_RB,
    T_LC,
    T_RC,
    T_COMMA,
    T_COLON,
    T_DOT,
    T_ASSIGN,
    T_PLUS,
    T_MINUS,
    T_STAR,
    T_SLASH,
    T_EQ,
    T_NE,
    T_LT,
    T_LE,
    T_GT,
    T_GE,
    T_PERCENT
} TT;

typedef struct {
    TT     t;
    char  *s;
    double num;
    int    line;
} TOK;

typedef struct {
    TOK  *v;
    int   n;
    int   cap;
    char *file;
} LEX;

static void tok_add(LEX *l, TT t, const char *s, double num, int line) {
    if (l->n == l->cap) {
        int  cap = l->cap ? l->cap * 2 : 64;
        TOK *v   = (TOK *)realloc(l->v, sizeof(TOK) * (size_t)cap);
        if (!v) return;

        l->v   = v;
        l->cap = cap;
    }

    l->v[l->n].t    = t;
    l->v[l->n].s    = s ? sys_dup(s) : NULL;
    l->v[l->n].num  = num;
    l->v[l->n].line = line;
    l->n++;
}

static void lex_free(LEX *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i].s);

    free(l->v);
    free(l->file);
    memset(l, 0, sizeof(*l));
}

static int star_lex(const char *src, const char *file, LEX *out, char *err, size_t errsz) {
    memset(out, 0, sizeof(*out));
    out->file        = sys_dup(file);
    const char *p    = src;
    int         line = 1;
    int         indents[64];
    int         depth = 0;
    indents[0]        = 0;
    int at_line_start = 1;
    int line_blank    = 0;
    int bdepth        = 0;
    while (*p) {
        if (at_line_start && !line_blank && bdepth == 0) {
            const char *q   = p;
            int         col = 0;
            while (*q == ' ') {
                q++;
                col++;
            }

            if (*q == '\t') {
                snprintf(err, errsz, "%s:%d: tabs are not allowed", file, line);
                return -1;
            }

            if (*q == '\n' || *q == '\r' || *q == '#') {
                p             = q;
                at_line_start = 0;
            } else {
                if (col > indents[depth]) {
                    if (depth >= 63) {
                        snprintf(err, errsz, "%s:%d: indent too deep", file, line);
                        return -1;
                    }
                    indents[++depth] = col;
                    tok_add(out, T_INDENT, NULL, 0, line);
                } else {
                    while (depth > 0 && col < indents[depth]) {
                        depth--;
                        tok_add(out, T_DEDENT, NULL, 0, line);
                    }

                    if (col != indents[depth]) {
                        snprintf(err, errsz, "%s:%d: bad dedent", file, line);
                        return -1;
                    }
                }

                p             = q;
                at_line_start = 0;
            }
        }

        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }

        if (*p == '#') {
            while (*p && *p != '\n') p++;
            continue;
        }

        if (*p == '\n') {
            if (bdepth == 0) tok_add(out, T_NEWLINE, NULL, 0, line);
            line++;
            p++;
            at_line_start = 1;
            line_blank    = 0;
            continue;
        }

        if (*p == '"' || *p == '\'') {
            char   q   = *p++;
            size_t cap = 32, n = 0;
            char  *buf = (char *)malloc(cap);
            if (!buf) {
                snprintf(err, errsz, "oom");
                return -1;
            }

            while (*p && *p != q) {
                char c = *p++;
                if (c == '\\' && *p) {
                    char e = *p++;
                    if (e == 'n') c = '\n';
                    else if (e == 't') c = '\t';
                    else c = e;
                }

                if (n + 1 >= cap) {
                    cap *= 2;
                    char *b2 = (char *)realloc(buf, cap);
                    if (!b2) {
                        free(buf);
                        snprintf(err, errsz, "oom");
                        return -1;
                    }
                    buf = b2;
                }

                buf[n++] = c;
            }

            if (*p != q) {
                free(buf);
                snprintf(err, errsz, "%s:%d: unterminated string", file, line);
                return -1;
            }

            p++;
            buf[n] = 0;
            tok_add(out, T_STR, buf, 0, line);
            free(buf);
            continue;
        }

        if (isdigit((unsigned char)*p)) {
            char  *end = NULL;
            double v   = strtod(p, &end);
            char   buf[64];
            size_t n = (size_t)(end - p);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;

            memcpy(buf, p, n);
            buf[n] = 0;
            tok_add(out, T_NUM, NULL, v, line);
            p = end;
            continue;
        }

        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *b = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;

            char   buf[256];
            size_t n = (size_t)(p - b);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;

            memcpy(buf, b, n);
            buf[n] = 0;
            tok_add(out, T_NAME, buf, 0, line);
            continue;
        }

        char c = *p++;
        switch (c) {
        case '(':
            bdepth++;
            tok_add(out, T_LP, NULL, 0, line);
            break;
        case ')':
            if (bdepth) bdepth--;
            tok_add(out, T_RP, NULL, 0, line);
            break;
        case '[':
            bdepth++;
            tok_add(out, T_LB, NULL, 0, line);
            break;
        case ']':
            if (bdepth) bdepth--;
            tok_add(out, T_RB, NULL, 0, line);
            break;
        case '{':
            bdepth++;
            tok_add(out, T_LC, NULL, 0, line);
            break;
        case '}':
            if (bdepth) bdepth--;
            tok_add(out, T_RC, NULL, 0, line);
            break;
        case ',': tok_add(out, T_COMMA, NULL, 0, line); break;
        case ':': tok_add(out, T_COLON, NULL, 0, line); break;
        case '.': tok_add(out, T_DOT, NULL, 0, line); break;
        case '+': tok_add(out, T_PLUS, NULL, 0, line); break;
        case '-': tok_add(out, T_MINUS, NULL, 0, line); break;
        case '*': tok_add(out, T_STAR, NULL, 0, line); break;
        case '/': tok_add(out, T_SLASH, NULL, 0, line); break;
        case '%': tok_add(out, T_PERCENT, NULL, 0, line); break;
        case '=':
            if (*p == '=') {
                p++;
                tok_add(out, T_EQ, NULL, 0, line);
            } else tok_add(out, T_ASSIGN, NULL, 0, line);
            break;
        case '!':
            if (*p == '=') {
                p++;
                tok_add(out, T_NE, NULL, 0, line);
            } else {
                snprintf(err, errsz, "%s:%d: unexpected '!'", file, line);
                return -1;
            }
            break;
        case '<':
            if (*p == '=') {
                p++;
                tok_add(out, T_LE, NULL, 0, line);
            } else tok_add(out, T_LT, NULL, 0, line);
            break;
        case '>':
            if (*p == '=') {
                p++;
                tok_add(out, T_GE, NULL, 0, line);
            } else tok_add(out, T_GT, NULL, 0, line);
            break;
        default: snprintf(err, errsz, "%s:%d: unexpected '%c'", file, line, c); return -1;
        }
    }

    tok_add(out, T_NEWLINE, NULL, 0, line);
    while (depth > 0) {
        depth--;
        tok_add(out, T_DEDENT, NULL, 0, line);
    }

    tok_add(out, T_EOF, NULL, 0, line);
    return 0;
}

#define MAX_VARS 512

typedef struct {
    char *name;
    STAR *val;
} BIND;

typedef struct {
    BIND v[MAX_VARS];
    int  n;
} SCOPE;

typedef struct {
    LEX       lex;
    int       pos;
    SCOPE    *scopes[64];
    int       nscope;
    STAR_CFG *cfg;
    char     *cwd;
    char      err[512];
    int       failed;
    int       loop_depth;
    STAR     *ret;
} INTERP;

static STAR *eval_expr(INTERP *I);
static STAR *eval_block(INTERP *I, int newline_terminated);

static void ierr(INTERP *I, const char *fmt, const char *a) {
    if (!I->failed) snprintf(I->err, sizeof(I->err), fmt, a);

    I->failed = 1;
}

static TOK *peek(INTERP *I) {
    return &I->lex.v[I->pos];
}
static TOK *peek2(INTERP *I) {
    return &I->lex.v[I->pos + 1];
}

static TOK *next(INTERP *I) {
    if (I->lex.v[I->pos].t != T_EOF) I->pos++;

    return &I->lex.v[I->pos - 1];
}

static int accept(INTERP *I, TT t) {
    if (peek(I)->t != t) return 0;

    next(I);
    return 1;
}

static SCOPE *scope_push(INTERP *I) {
    if (I->nscope >= 64) return NULL;

    SCOPE *s = (SCOPE *)calloc(1, sizeof(SCOPE));
    if (s) I->scopes[I->nscope++] = s;

    return s;
}

static void scope_pop(INTERP *I) {
    if (I->nscope <= 0) return;

    SCOPE *s = I->scopes[--I->nscope];
    for (int i = 0; i < s->n; i++) {
        free(s->v[i].name);
        star_free(s->v[i].val);
    }

    free(s);
}

static STAR *var_get(INTERP *I, const char *name) {
    for (int i = I->nscope - 1; i >= 0; i--)
        for (int k = 0; k < I->scopes[i]->n; k++)
            if (!strcmp(I->scopes[i]->v[k].name, name)) return I->scopes[i]->v[k].val;

    return NULL;
}

static int var_set(INTERP *I, const char *name, STAR *val) {
    for (int i = I->nscope - 1; i >= 0; i--)
        for (int k = 0; k < I->scopes[i]->n; k++)
            if (!strcmp(I->scopes[i]->v[k].name, name)) {
                star_free(I->scopes[i]->v[k].val);
                I->scopes[i]->v[k].val = val;
                return 0;
            }

    SCOPE *s = I->scopes[I->nscope - 1];
    if (s->n >= MAX_VARS) return -1;

    s->v[s->n].name = sys_dup(name);
    s->v[s->n].val  = val;
    s->n++;
    return 0;
}

static int truthy(const STAR *v) {
    if (!v) return 0;
    if (v->kind == STAR_BOOL) return v->b;
    if (v->kind == STAR_NUM) return v->num != 0;
    if (v->kind == STAR_STR) return v->s[0] != 0;
    if (v->kind == STAR_LIST || v->kind == STAR_DICT) return v->n != 0;
    if (v->kind == STAR_NONE) return 0;

    return 1;
}

STAR *star_userfn(char **params, int nparams, int body_pos) {
    STAR *v = alloc(STAR_USERFN);
    if (!v) return star_none();

    v->params   = (char **)calloc((size_t)(nparams ? nparams : 1), sizeof(char *));
    v->nparams  = nparams;
    v->body_pos = body_pos;
    for (int i = 0; i < nparams; i++) v->params[i] = sys_dup(params[i]);

    return v;
}

static STAR *call_builtin(INTERP *I, const char *name, STAR *args, STAR *kw);

static STAR *eval_primary(INTERP *I);

static STAR *eval_call(INTERP *I, const char *name, STAR *args, STAR *kw) {
    STAR *fn = var_get(I, name);
    if (fn && fn->kind == STAR_USERFN) {
        SCOPE *s = scope_push(I);
        if (!s) {
            ierr(I, "too deep", "");
            return star_none();
        }

        for (int i = 0; i < fn->nparams; i++) {
            STAR *v = i < args->n ? star_dup(args->items[i]) : star_none();
            var_set(I, fn->params[i], v);
        }

        int save = I->pos;
        I->pos   = fn->body_pos;
        STAR *r  = eval_block(I, 0);
        if (I->ret) {
            r      = I->ret;
            I->ret = NULL;
        }

        I->pos = save;
        scope_pop(I);
        return r ? r : star_none();
    }

    STAR *r = call_builtin(I, name, args, kw);
    return r ? r : star_none();
}

static STAR *eval_args(INTERP *I, STAR **kw_out) {
    STAR *args = star_list();
    STAR *kw   = star_dict();
    *kw_out    = kw;

    if (accept(I, T_RP)) return args;

    for (;;) {
        if (peek(I)->t == T_NAME && peek2(I)->t == T_ASSIGN) {
            char key[256];
            snprintf(key, sizeof(key), "%s", peek(I)->s);
            next(I);
            next(I);
            STAR *v = eval_expr(I);
            star_dict_put(kw, key, v);
        } else {
            STAR *v = eval_expr(I);
            star_list_add(args, v);
        }

        if (accept(I, T_COMMA)) continue;

        accept(I, T_RP);
        break;
    }

    return args;
}

static STAR *eval_index(INTERP *I, STAR *base) {
    if (peek(I)->t == T_COLON) {
        next(I);
        int hi = base->n;
        if (peek(I)->t != T_RB) {
            STAR *hv = eval_expr(I);
            hi       = (int)star_as_num(hv, base->n);
            star_free(hv);
        }

        accept(I, T_RB);
        STAR *l = star_list();
        for (int i = 0; i < hi && i < base->n; i++) star_list_add(l, star_dup(base->items[i]));

        return l;
    }

    STAR *idx = eval_expr(I);
    if (accept(I, T_COLON)) {
        int lo = (int)star_as_num(idx, 0);
        int hi = base->n;
        if (peek(I)->t != T_RB) {
            STAR *hv = eval_expr(I);
            hi       = (int)star_as_num(hv, base->n);
            star_free(hv);
        }

        accept(I, T_RB);
        star_free(idx);
        STAR *l = star_list();
        for (int i = lo; i < hi && i < base->n; i++) star_list_add(l, star_dup(base->items[i]));

        return l;
    }

    accept(I, T_RB);
    if (base->kind == STAR_DICT && idx->kind == STAR_STR) {
        STAR *v = star_get(base, idx->s);
        star_free(idx);
        return star_dup(v ? v : star_none());
    }

    if (base->kind == STAR_LIST) {
        int   i = (int)star_as_num(idx, 0);
        STAR *v = star_at(base, i);
        star_free(idx);
        return star_dup(v ? v : star_none());
    }

    star_free(idx);
    return star_none();
}

static STAR *eval_unary(INTERP *I) {
    if (accept(I, T_MINUS)) {
        STAR *v = eval_unary(I);
        if (v->kind == STAR_NUM) {
            STAR *r = star_num(-v->num);
            star_free(v);
            return r;
        }

        return v;
    }

    return eval_primary(I);
}

static STAR *postfix(INTERP *I, STAR *v) {
    for (;;) {
        if (accept(I, T_DOT)) {
            if (peek(I)->t != T_NAME) return v;

            char m[256];
            snprintf(m, sizeof(m), "%s", peek(I)->s);
            next(I);
            accept(I, T_LP) ? accept(I, T_RP) : 0;
            if (!strcmp(m, "keys") && v->kind == STAR_DICT) {
                STAR *l = star_list();
                for (int i = 0; i < v->n; i++) star_list_add(l, star_str(v->keys[i]));

                star_free(v);
                v = l;
                continue;
            }

            if (!strcmp(m, "items") && v->kind == STAR_DICT) {
                STAR *l = star_list();
                for (int i = 0; i < v->n; i++) {
                    STAR *pair = star_list();
                    star_list_add(pair, star_str(v->keys[i]));
                    star_list_add(pair, star_dup(v->vals[i]));
                    star_list_add(l, pair);
                }

                star_free(v);
                v = l;
                continue;
            }

            if (!strcmp(m, "values") && v->kind == STAR_DICT) {
                STAR *l = star_list();
                for (int i = 0; i < v->n; i++) star_list_add(l, star_dup(v->vals[i]));

                star_free(v);
                v = l;
                continue;
            }

            continue;
        }

        if (peek(I)->t == T_LB) {
            next(I);
            STAR *r = eval_index(I, v);
            star_free(v);
            v = r;
            continue;
        }

        break;
    }

    return v;
}

static int cmp_vals(const STAR *a, const STAR *b) {
    if (a->kind == STAR_NUM && b->kind == STAR_NUM)
        return a->num < b->num ? -1 : a->num > b->num ? 1 : 0;

    if (a->kind == STAR_STR && b->kind == STAR_STR) {
        int c = strcmp(a->s, b->s);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }

    return 0;
}

static STAR *eval_mul(INTERP *I) {
    STAR *v = eval_unary(I);
    for (;;) {
        TT t = peek(I)->t;
        if (t != T_STAR && t != T_SLASH && t != T_PERCENT) return v;

        next(I);
        STAR  *r = eval_unary(I);
        double a = star_as_num(v, 0), b = star_as_num(r, 0);
        double o;
        if (t == T_STAR) o = a * b;
        else if (t == T_SLASH) o = b ? a / b : 0;
        else o = b ? a - b * (double)(long long)(a / b) : 0;

        star_free(v);
        star_free(r);
        v = star_num(o);
    }
}

static STAR *eval_add(INTERP *I) {
    STAR *v = eval_mul(I);
    for (;;) {
        TT t = peek(I)->t;
        if (t != T_PLUS && t != T_MINUS) return v;

        next(I);
        STAR *r = eval_mul(I);
        if (t == T_PLUS) {
            star_add(v, r);
            star_free(r);
        } else {
            double o = star_as_num(v, 0) - star_as_num(r, 0);
            star_free(v);
            star_free(r);
            v = star_num(o);
        }
    }
}

static STAR *eval_cmp(INTERP *I) {
    STAR *v = eval_add(I);
    for (;;) {
        TT t = peek(I)->t;
        if (t != T_EQ && t != T_NE && t != T_LT && t != T_LE && t != T_GT && t != T_GE) return v;

        next(I);
        STAR *r = eval_add(I);
        int   c = cmp_vals(v, r);
        int   o = 0;
        if (t == T_EQ) o = c == 0 && v->kind == r->kind;
        else if (t == T_NE) o = !(c == 0 && v->kind == r->kind);
        else if (t == T_LT) o = c < 0;
        else if (t == T_LE) o = c <= 0;
        else if (t == T_GT) o = c > 0;
        else if (t == T_GE) o = c >= 0;

        star_free(v);
        star_free(r);
        v = star_bool(o);
    }
}

static STAR *eval_expr(INTERP *I) {
    if (I->failed) return star_none();

    return eval_cmp(I);
}

static STAR *eval_primary(INTERP *I) {
    TOK *t = peek(I);
    if (t->t == T_NUM) {
        next(I);
        return star_num(t->num);
    }
    if (t->t == T_STR) {
        next(I);
        return star_str(t->s);
    }

    if (t->t == T_LB) {
        next(I);
        STAR *l = star_list();
        if (accept(I, T_RB)) return postfix(I, l);

        for (;;) {
            star_list_add(l, eval_expr(I));
            if (accept(I, T_COMMA)) {
                if (peek(I)->t == T_RB) {
                    next(I);
                    break;
                }
                continue;
            }

            accept(I, T_RB);
            break;
        }

        return postfix(I, l);
    }

    if (t->t == T_LC) {
        next(I);
        STAR *d = star_dict();
        if (accept(I, T_RC)) return postfix(I, d);

        for (;;) {
            STAR *k = eval_expr(I);
            accept(I, T_COLON);
            STAR *v = eval_expr(I);
            char  kb[512];
            snprintf(kb, sizeof(kb), "%s", star_as_str(k) ? star_as_str(k) : "");
            star_dict_put(d, kb, v);
            star_free(k);
            if (accept(I, T_COMMA)) {
                if (peek(I)->t == T_RC) {
                    next(I);
                    break;
                }
                continue;
            }

            accept(I, T_RC);
            break;
        }

        return postfix(I, d);
    }

    if (t->t == T_LP) {
        next(I);
        STAR *v = eval_expr(I);
        if (peek(I)->t == T_COMMA) {
            STAR *l = star_list();
            star_list_add(l, v);
            while (accept(I, T_COMMA)) {
                if (peek(I)->t == T_RP) break;

                star_list_add(l, eval_expr(I));
            }

            accept(I, T_RP);
            return postfix(I, l);
        }

        accept(I, T_RP);
        return v;
    }

    if (t->t == T_NAME) {
        next(I);
        if (!strcmp(t->s, "True")) return star_bool(1);
        if (!strcmp(t->s, "False")) return star_bool(0);
        if (!strcmp(t->s, "None")) return star_none();

        if (peek(I)->t == T_LP) {
            next(I);
            STAR *kw   = NULL;
            STAR *args = eval_args(I, &kw);
            STAR *r    = eval_call(I, t->s, args, kw);
            star_free(args);
            star_free(kw);
            return postfix(I, r);
        }

        STAR *v = var_get(I, t->s);
        return postfix(I, star_dup(v ? v : star_none()));
    }

    return star_none();
}

typedef struct PAT PAT;

struct PAT {
    char *name;
    int   nsub;
    PAT  *sub;
};

static int parse_pattern(INTERP *I, PAT *p) {
    memset(p, 0, sizeof(*p));
    if (accept(I, T_LP)) {
        for (;;) {
            PAT sub;
            if (!parse_pattern(I, &sub)) return 0;

            PAT *ns = (PAT *)realloc(p->sub, sizeof(PAT) * (size_t)(p->nsub + 1));
            if (!ns) return 0;

            p->sub            = ns;
            p->sub[p->nsub++] = sub;
            if (accept(I, T_COMMA)) continue;

            accept(I, T_RP);
            break;
        }

        return 1;
    }

    if (peek(I)->t == T_NAME) {
        p->name = peek(I)->s;
        next(I);
        return 1;
    }

    return 0;
}

static void free_pattern(PAT *p) {
    for (int i = 0; i < p->nsub; i++) free_pattern(&p->sub[i]);

    free(p->sub);
}

static void bind_pattern(INTERP *I, const PAT *p, const STAR *v) {
    if (p->name) {
        var_set(I, p->name, star_dup(v ? v : star_none()));
        return;
    }

    if (!v || v->kind != STAR_LIST) return;

    for (int i = 0; i < p->nsub; i++) bind_pattern(I, &p->sub[i], i < v->n ? v->items[i] : NULL);
}

static int is_unsupported(const char *w) {
    static const char *bad[] = {
        "while", "import", "class",    "try",   "with",  "lambda", "assert",
        "del",   "global", "nonlocal", "yield", "raise", "except", "finally",
        "match", "async",  "await",    "exec",  "eval",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (!strcmp(w, bad[i])) return 1;

    return 0;
}

static void skip_block(INTERP *I) {
    accept(I, T_NEWLINE);
    int depth = 0;
    while (peek(I)->t != T_EOF) {
        if (peek(I)->t == T_INDENT) {
            depth++;
            next(I);
        } else if (peek(I)->t == T_DEDENT) {
            next(I);
            if (--depth <= 0) break;
        } else next(I);
    }
}

static void skip_line(INTERP *I) {
    while (peek(I)->t != T_NEWLINE && peek(I)->t != T_EOF && peek(I)->t != T_DEDENT) next(I);

    accept(I, T_NEWLINE);
}

static STAR *eval_block(INTERP *I, int newline_terminated);

static STAR *eval_block_top(INTERP *I) {
    return eval_block(I, -1);
}

static STAR *eval_block(INTERP *I, int newline_terminated) {
    if (newline_terminated > 0) accept(I, T_NEWLINE);

    if (newline_terminated >= 0 && !accept(I, T_INDENT)) {
        ierr(I, "%s: expected indented block", I->lex.file);
        return star_none();
    }

    STAR *last = star_none();
    while (!I->failed && !I->ret && peek(I)->t != T_DEDENT && peek(I)->t != T_EOF) {
        if (accept(I, T_NEWLINE)) continue;

        TOK *t = peek(I);
        if (t->t == T_NAME && !strcmp(t->s, "def")) {
            next(I);
            TOK *nm = next(I);
            if (nm->t != T_NAME) {
                ierr(I, "bad def", "");
                break;
            }

            char *params[32];
            int   np = 0;
            accept(I, T_LP);
            if (!accept(I, T_RP)) {
                for (;;) {
                    TOK *p = next(I);
                    if (p->t == T_NAME && np < 32) params[np++] = p->s;

                    if (accept(I, T_COMMA)) continue;

                    accept(I, T_RP);
                    break;
                }
            }

            accept(I, T_COLON);
            accept(I, T_NEWLINE);
            int body = I->pos;
            accept(I, T_INDENT);
            int depth = 1;
            while (depth > 0 && peek(I)->t != T_EOF) {
                if (accept(I, T_INDENT)) depth++;
                else if (accept(I, T_DEDENT)) depth--;
                else next(I);
            }

            var_set(I, nm->s, star_userfn(params, np, body));
            continue;
        }

        if (t->t == T_NAME && !strcmp(t->s, "if")) {
            next(I);
            int done = 0;
            for (;;) {
                if (done) {
                    skip_block(I);
                } else {
                    STAR *c = eval_expr(I);
                    accept(I, T_COLON);
                    if (truthy(c)) {
                        star_free(last);
                        last = eval_block(I, 1);
                        done = 1;
                    } else skip_block(I);

                    star_free(c);
                }

                if (peek(I)->t != T_NAME) break;

                if (!strcmp(peek(I)->s, "elif")) {
                    next(I);
                    continue;
                }

                if (!strcmp(peek(I)->s, "else")) {
                    next(I);
                    accept(I, T_COLON);
                    if (!done) {
                        star_free(last);
                        last = eval_block(I, 1);
                        done = 1;
                    } else {
                        skip_block(I);
                    }
                }

                break;
            }

            continue;
        }

        if (t->t == T_NAME && !strcmp(t->s, "for")) {
            next(I);
            PAT pat;
            memset(&pat, 0, sizeof(pat));
            for (;;) {
                PAT one;
                if (!parse_pattern(I, &one)) {
                    free_pattern(&pat);
                    ierr(I, "for: bad loop variable", "");
                    goto for_done;
                }

                PAT *ns = (PAT *)realloc(pat.sub, sizeof(PAT) * (size_t)(pat.nsub + 1));
                if (!ns) {
                    free_pattern(&one);
                    free_pattern(&pat);
                    break;
                }

                pat.sub             = ns;
                pat.sub[pat.nsub++] = one;
                if (accept(I, T_COMMA)) continue;

                break;
            }

            if (pat.nsub == 1 && pat.sub[0].name) {
                pat.name = pat.sub[0].name;
                free(pat.sub);
                pat.sub  = NULL;
                pat.nsub = 0;
            }

            next(I);
            STAR *it = eval_expr(I);
            accept(I, T_COLON);
            accept(I, T_NEWLINE);
            if (peek(I)->t != T_INDENT) {
                ierr(I, "for: need block", "");
                break;
            }

            int body  = I->pos;
            int depth = 1;
            accept(I, T_INDENT);
            while (depth > 0 && peek(I)->t != T_EOF) {
                if (accept(I, T_INDENT)) depth++;
                else if (accept(I, T_DEDENT)) depth--;
                else next(I);
            }

            if (it->kind == STAR_DICT || it->kind == STAR_LIST) {
                for (int i = 0; i < it->n && !I->failed && !I->ret; i++) {
                    scope_push(I);
                    if (it->kind == STAR_DICT) {
                        bind_pattern(I, &pat, star_str(it->keys[i]));
                        if (pat.nsub || !pat.name) {
                            STAR *pair = star_list();
                            star_list_add(pair, star_str(it->keys[i]));
                            star_list_add(pair, star_dup(it->vals[i]));
                            bind_pattern(I, &pat, pair);
                            star_free(pair);
                        } else {
                            var_set(I, pat.name, star_str(it->keys[i]));
                        }
                    } else {
                        bind_pattern(I, &pat, it->items[i]);
                    }

                    int save = I->pos;
                    I->pos   = body;
                    I->loop_depth++;
                    star_free(last);
                    last = eval_block(I, 0);
                    I->loop_depth--;
                    I->pos = save;
                    scope_pop(I);
                }
            }

            star_free(it);
            free_pattern(&pat);
        for_done:
            continue;
        }

        if (t->t == T_NAME && !strcmp(t->s, "return")) {
            next(I);
            if (peek(I)->t == T_NEWLINE) I->ret = star_none();
            else I->ret = eval_expr(I);

            skip_line(I);
            break;
        }

        if (t->t == T_NAME && is_unsupported(t->s)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s:%d: '%s' is not supported", I->lex.file, t->line, t->s);
            ierr(I, "%s", msg);
            break;
        }

        if (t->t == T_NAME && !strcmp(t->s, "pass")) {
            next(I);
            skip_line(I);
            continue;
        }

        if (t->t == T_NAME && !strcmp(t->s, "break")) {
            next(I);
            skip_line(I);
            continue;
        }

        if (t->t == T_NAME && peek2(I)->t == T_ASSIGN) {
            char name[256];
            snprintf(name, sizeof(name), "%s", t->s);
            next(I);
            next(I);
            STAR *v = eval_expr(I);
            var_set(I, name, v);
            skip_line(I);
            continue;
        }

        STAR *ev = eval_expr(I);
        star_free(ev);
        skip_line(I);
    }

    accept(I, T_DEDENT);
    return last;
}

static STAR *b_len(INTERP *, STAR *a, STAR *) {
    return star_num(a->n ? star_len(a->items[0]) : 0);
}

static STAR *b_range(INTERP *, STAR *a, STAR *) {
    int lo = 0, hi = 0;
    if (a->n == 1) hi = (int)star_as_num(a->items[0], 0);
    else if (a->n >= 2) {
        lo = (int)star_as_num(a->items[0], 0);
        hi = (int)star_as_num(a->items[1], 0);
    }

    STAR *l = star_list();
    for (int i = lo; i < hi; i++) star_list_add(l, star_num(i));

    return l;
}

static STAR *b_str(INTERP *, STAR *a, STAR *) {
    STAR *v = a->n ? a->items[0] : star_none();
    if (v->kind == STAR_STR) return star_dup(v);
    if (v->kind == STAR_BOOL) return star_str(v->b ? "True" : "False");
    if (v->kind == STAR_NUM) return star_str(num_str(v->num));
    if (v->kind == STAR_NONE) return star_str("None");

    return star_str("");
}

static STAR *b_glob(INTERP *I, STAR *a, STAR *) {
    STAR       *l    = star_list();
    const char *base = I->cwd ? I->cwd : ".";
    for (int i = 0; i < a->n; i++) {
        const char *pat = star_as_str(a->items[i]);
        if (!pat) continue;

        char       *dir    = NULL;
        const char *gp     = pat;
        const char *star_c = strpbrk(pat, "*?");
        if (star_c) {
            const char *slash = NULL;
            for (const char *q = pat; q < star_c; q++)
                if (*q == '/') slash = q;

            if (slash) {
                size_t n = (size_t)(slash - pat) + strlen(base) + 2;
                dir      = (char *)malloc(n);
                if (dir) snprintf(dir, n, "%s/%.*s", base, (int)(slash - pat), pat);
                gp = slash + 1;
            }
        }

        GLOB_LIST g;
        glob_init(&g);
        glob_dir(dir ? dir : base, gp, &g);
        for (int k = 0; k < g.n; k++) {
            if (dir) {
                char *full = (char *)malloc(strlen(dir) + strlen(g.items[k]) + 2);
                if (full) {
                    sprintf(full, "%s/%s", dir, g.items[k]);
                    star_list_add(l, star_str(full));
                    free(full);
                }
            } else {
                star_list_add(l, star_str(g.items[k]));
            }
        }

        glob_free(&g);
        free(dir);
    }

    return l;
}

static void cfg_at(char ***v, int idx, const char *s) {
    char **next = (char **)realloc(*v, sizeof(char *) * (size_t)(idx + 1));
    if (!next) return;

    *v        = next;
    (*v)[idx] = sys_dup(s ? s : "");
}

static char *kw_dup(STAR *kw, const char *k) {
    STAR       *v = star_get(kw, k);
    const char *s = v ? star_as_str(v) : NULL;
    return s ? sys_dup(s) : NULL;
}

static const char *kw_str(STAR *kw, const char *k) {
    STAR *v = star_get(kw, k);
    return v ? star_as_str(v) : NULL;
}

static void set(char **slot, STAR *kw, const char *key) {
    const char *v = kw_str(kw, key);
    if (!v) return;

    free(*slot);
    *slot = sys_dup(v);
}

static STAR *obj_new(const char *kind, const char *name) {
    STAR *d = star_dict();
    star_dict_put(d, "__kind", star_str(kind));
    star_dict_put(d, "name", star_str(name ? name : ""));
    return d;
}

static const char *obj_name(const STAR *v) {
    if (!v) return NULL;

    if (v->kind == STAR_STR) return v->s;

    if (v->kind == STAR_DICT) {
        STAR *n = star_get(v, "name");
        if (n && n->kind == STAR_STR) return n->s;
    }

    return NULL;
}

static STAR *h_project(INTERP *I, STAR *, STAR *kw) {
    set(&I->cfg->project_name, kw, "name");
    set(&I->cfg->build_dir, kw, "build_dir");
    set(&I->cfg->toolchain, kw, "default_toolchain");
    set(&I->cfg->store, kw, "store");
    return star_none();
}

static STAR *h_build(INTERP *I, STAR *, STAR *kw) {
    set(&I->cfg->build_dir, kw, "dir");
    set(&I->cfg->toolchain, kw, "toolchain");
    set(&I->cfg->store, kw, "store");
    return star_none();
}

static STAR *h_target_config(INTERP *I, STAR *, STAR *kw) {
    set(&I->cfg->arch, kw, "arch");
    set(&I->cfg->abi, kw, "abi");
    set(&I->cfg->flt, kw, "float");
    set(&I->cfg->sysroot, kw, "sysroot");
    return star_none();
}

static STAR *h_toolchain(INTERP *I, STAR *, STAR *kw) {
    const char *name = kw_str(kw, "name");
    const char *cc   = kw_str(kw, "cc");
    const char *base = kw_str(kw, "based_on");
    if (name) {
        int i = I->cfg->ntc++;
        cfg_at(&I->cfg->tc_name, i, name);
        cfg_at(&I->cfg->tc_cc, i, cc ? cc : "");
        cfg_at(&I->cfg->tc_based, i, base ? base : "");
    }

    return obj_new("toolchain", name);
}

static STAR *h_platform(INTERP *I, STAR *, STAR *kw) {
    const char *name = kw_str(kw, "name");
    if (!name) {
        ierr(I, "platform(): missing name", "");
        return star_none();
    }

    STAR_PLATFORM p;
    memset(&p, 0, sizeof(p));
    p.name      = sys_dup(name);
    p.arch      = kw_dup(kw, "arch");
    p.abi       = kw_dup(kw, "abi");
    p.flt       = kw_dup(kw, "float");
    p.sysroot   = kw_dup(kw, "sysroot");
    p.toolchain = kw_dup(kw, "toolchain");
    STAR_PLATFORM *np =
        (STAR_PLATFORM *)realloc(I->cfg->pl, sizeof(STAR_PLATFORM) * (size_t)(I->cfg->npl + 1));
    if (!np) {
        ierr(I, "out of memory", "");
        return star_none();
    }

    I->cfg->pl                = np;
    I->cfg->pl[I->cfg->npl++] = p;
    return obj_new("platform", name);
}

static STAR *h_package(INTERP *I, STAR *, STAR *kw) {
    const char *name = kw_str(kw, "name");
    const char *ver  = kw_str(kw, "version");
    const char *src  = kw_str(kw, "source");
    if (name) {
        int i = I->cfg->ndep++;
        cfg_at(&I->cfg->dep_name, i, name);
        cfg_at(&I->cfg->dep_ver, i, ver ? ver : "latest");
        cfg_at(&I->cfg->dep_src, i, src ? src : "");
    }

    return obj_new("package", name);
}

STAR_TARGET *star_cfg_add_target(STAR_CFG *c, const char *name) {
    STAR_TARGET *tg = (STAR_TARGET *)realloc(c->tg, sizeof(STAR_TARGET) * (size_t)(c->ntg + 1));
    if (!tg) return NULL;

    c->tg = tg;
    memset(&c->tg[c->ntg], 0, sizeof(STAR_TARGET));
    c->tg[c->ntg].name = sys_dup(name);
    c->ntg++;
    return &c->tg[c->ntg - 1];
}

static const char *type_name(const char *t) {
    if (!t) return "exe";
    if (!strcmp(t, "lib") || !strcmp(t, "staticlib")) return "staticlib";
    if (!strcmp(t, "shared") || !strcmp(t, "sharedlib")) return "sharedlib";
    if (!strcmp(t, "raw")) return "raw";
    if (!strcmp(t, "custom")) return "custom";

    return "exe";
}

static void tg_list(STAR *kw, const char *key, char ***out, int *n) {
    STAR *v = star_get(kw, key);
    if (!v || v->kind != STAR_LIST) return;

    for (int i = 0; i < v->n; i++) {
        const char *sv = star_as_str(v->items[i]);
        if (!sv) continue;

        cfg_at(out, (*n)++, sv);
    }
}

static STAR *h_install(INTERP *I, STAR *, STAR *kw) {
    const char *name = kw_str(kw, "target");
    if (!name) {
        ierr(I, "install(): missing target", "");
        return star_none();
    }

    STAR_TARGET *t = NULL;
    for (int i = 0; i < I->cfg->ntg; i++)
        if (!strcmp(I->cfg->tg[i].name, name)) t = &I->cfg->tg[i];

    if (!t) {
        ierr(I, "install(): unknown target", "");
        return star_none();
    }

    tg_list(kw, "bin", &t->i_bin, &t->n_i_bin);
    tg_list(kw, "lib", &t->i_lib, &t->n_i_lib);
    tg_list(kw, "include", &t->i_include, &t->n_i_include);
    tg_list(kw, "share", &t->i_share, &t->n_i_share);
    tg_list(kw, "etc", &t->i_etc, &t->n_i_etc);
    t->i_rootfs = kw_dup(kw, "rootfs");
    return star_none();
}

static void tg_deps(STAR *kw, STAR_TARGET *t) {
    STAR *v = star_get(kw, "deps");
    if (!v || v->kind != STAR_LIST) return;

    for (int i = 0; i < v->n; i++) {
        STAR       *item = v->items[i];
        const char *n    = obj_name(item);
        if (!n) continue;

        STAR *k = item->kind == STAR_DICT ? star_get(item, "__kind") : NULL;
        if (k && k->kind == STAR_STR && !strcmp(k->s, "package"))
            cfg_at(&t->pkgdeps, t->npkgdeps++, n);
        else cfg_at(&t->deps, t->ndeps++, n);
    }
}

static char *ref_name(STAR *kw, const char *key) {
    STAR *v = star_get(kw, key);
    if (!v) return NULL;

    const char *n = obj_name(v);
    return n ? sys_dup(n) : NULL;
}

static STAR *h_target(INTERP *I, STAR *, STAR *kw) {
    const char *name = kw_str(kw, "name");
    if (!name) {
        ierr(I, "target(): missing name", "");
        return star_none();
    }

    STAR_TARGET *t = star_cfg_add_target(I->cfg, name);
    if (!t) {
        ierr(I, "out of memory", "");
        return star_none();
    }

    t->type = sys_dup(type_name(kw_str(kw, "type")));
    tg_list(kw, "src", &t->src, &t->nsrc);
    tg_list(kw, "inc", &t->inc, &t->ninc);
    tg_deps(kw, t);
    tg_list(kw, "cflags", &t->cflags, &t->ncflags);
    tg_list(kw, "ldflags", &t->ldflags, &t->nldflags);
    t->ldscript = kw_dup(kw, "linker_script");
    t->entry    = kw_dup(kw, "entry");
    t->out      = kw_dup(kw, "out");
    t->platform = ref_name(kw, "platform");
    return star_none();
}

static STAR *call_builtin(INTERP *I, const char *name, STAR *args, STAR *kw) {
    if (!strcmp(name, "len")) return b_len(I, args, kw);
    if (!strcmp(name, "range")) return b_range(I, args, kw);
    if (!strcmp(name, "str")) return b_str(I, args, kw);
    if (!strcmp(name, "glob")) return b_glob(I, args, kw);
    if (!strcmp(name, "build")) return h_build(I, args, kw);
    if (!strcmp(name, "project")) return h_project(I, args, kw);
    if (!strcmp(name, "platform")) return h_platform(I, args, kw);
    if (!strcmp(name, "target_config")) return h_target_config(I, args, kw);
    if (!strcmp(name, "toolchain")) return h_toolchain(I, args, kw);
    if (!strcmp(name, "package")) return h_package(I, args, kw);
    if (!strcmp(name, "target")) return h_target(I, args, kw);
    if (!strcmp(name, "install")) return h_install(I, args, kw);

    char msg[256];
    snprintf(msg, sizeof(msg), "%s: unknown function '%s'", I->lex.file, name);
    ierr(I, "%s", msg);
    return star_none();
}

void star_cfg_free(STAR_CFG *c) {
    free(c->root);
    free(c->file);
    free(c->project_name);
    for (int i = 0; i < c->npl; i++) {
        free(c->pl[i].name);
        free(c->pl[i].arch);
        free(c->pl[i].abi);
        free(c->pl[i].flt);
        free(c->pl[i].sysroot);
        free(c->pl[i].toolchain);
    }

    free(c->pl);
    free(c->build_dir);
    free(c->toolchain);
    free(c->store);
    free(c->arch);
    free(c->abi);
    free(c->flt);
    free(c->sysroot);
    for (int i = 0; i < c->ntc; i++) {
        free(c->tc_name[i]);
        free(c->tc_cc[i]);
        free(c->tc_based[i]);
    }

    free(c->tc_name);
    free(c->tc_cc);
    free(c->tc_based);
    for (int i = 0; i < c->ndep; i++) {
        free(c->dep_name[i]);
        free(c->dep_ver[i]);
        free(c->dep_src[i]);
    }

    free(c->dep_name);
    free(c->dep_ver);
    free(c->dep_src);
    for (int i = 0; i < c->ntg; i++) {
        STAR_TARGET *t = &c->tg[i];
        free(t->name);
        free(t->type);
        free(t->ldscript);
        free(t->entry);
        free(t->out);
        free(t->platform);
        for (int k = 0; k < t->nsrc; k++) free(t->src[k]);
        for (int k = 0; k < t->ninc; k++) free(t->inc[k]);
        for (int k = 0; k < t->ndeps; k++) free(t->deps[k]);
        for (int k = 0; k < t->npkgdeps; k++) free(t->pkgdeps[k]);
        for (int k = 0; k < t->ncflags; k++) free(t->cflags[k]);
        for (int k = 0; k < t->nldflags; k++) free(t->ldflags[k]);
        for (int k = 0; k < t->n_i_bin; k++) free(t->i_bin[k]);
        for (int k = 0; k < t->n_i_lib; k++) free(t->i_lib[k]);
        for (int k = 0; k < t->n_i_include; k++) free(t->i_include[k]);
        for (int k = 0; k < t->n_i_share; k++) free(t->i_share[k]);
        for (int k = 0; k < t->n_i_etc; k++) free(t->i_etc[k]);

        free(t->i_bin);
        free(t->i_lib);
        free(t->i_include);
        free(t->i_share);
        free(t->i_etc);
        free(t->i_rootfs);
        free(t->src);
        free(t->inc);
        free(t->deps);
        free(t->pkgdeps);
        free(t->cflags);
        free(t->ldflags);
    }

    free(c->tg);
    memset(c, 0, sizeof(*c));
}

static char *slurp(const char *path, char *err, size_t errsz) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "cannot read %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        snprintf(err, errsz, "out of memory");
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got]   = 0;
    fclose(f);
    return buf;
}

int star_run(const char *path, STAR_CFG *cfg, char *err, size_t errsz) {
    char *src = slurp(path, err, errsz);
    if (!src) return -1;

    star_init();
    INTERP I;
    memset(&I, 0, sizeof(I));
    I.cfg = cfg;
    I.cwd = cfg->root;
    if (star_lex(src, path, &I.lex, err, errsz) != 0) {
        free(src);
        return -1;
    }

    free(src);
    scope_push(&I);
    eval_block_top(&I);
    int failed = I.failed;
    if (failed) snprintf(err, errsz, "%s", I.err);

    scope_pop(&I);
    lex_free(&I.lex);
    return failed ? -1 : 0;
}
