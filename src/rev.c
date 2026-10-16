/* ============================================================================
 * Reverie Engine — rev.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "rev.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"

/* --------------------------------------------------------------------------
 * lexer
 * ------------------------------------------------------------------------ */

typedef enum {
        TK_EOF = 0,
        TK_IDENT,
        TK_NUM,      /* num + unit ('%', 's', 'm' for ms, 0) */
        TK_STR,
        TK_COLOR,    /* #... parsed into col */
        TK_LBRACE, TK_RBRACE,
        TK_LPAREN, TK_RPAREN,
        TK_LBRACKET, TK_RBRACKET,
        TK_COMMA, TK_SEMI, TK_EQ
} TokKind;

typedef struct {
        TokKind kind;
        char   *s;       /* IDENT / STR text (arena) */
        f64     num;
        char    unit;    /* 0, '%', 's', 'm' */
        RColor  col;
        int     line, col_pos;
} Token;

typedef struct {
        const char *p, *end;
        int line, col;
        RArena *ar;
        Token cur;
        bool  err;
        char  errmsg[192];
} Lex;

static bool lex_fail(Lex *lx, const char *fmt, ...)
{
        if (!lx->err) {
                va_list ap;
                va_start(ap, fmt);
                vsnprintf(lx->errmsg, sizeof lx->errmsg, fmt, ap);
                va_end(ap);
                lx->err = true;
        }
        return false;
}

static char lex_peek(Lex *lx)  { return lx->p < lx->end ? *lx->p : '\0'; }
static char lex_next(Lex *lx)
{
        if (lx->p >= lx->end)
                return '\0';
        char c = *lx->p++;
        if (c == '\n') { lx->line++; lx->col = 1; } else { lx->col++; }
        return c;
}

static void lex_ws(Lex *lx)
{
        for (;;) {
                while (lx->p < lx->end && isspace((unsigned char)*lx->p))
                        lex_next(lx);
                if (lx->p + 1 < lx->end && lx->p[0] == '/' && lx->p[1] == '/') {
                        while (lx->p < lx->end && *lx->p != '\n')
                                lex_next(lx);
                        continue;
                }
                if (lx->p + 1 < lx->end && lx->p[0] == '/' && lx->p[1] == '*') {
                        lex_next(lx); lex_next(lx);
                        while (lx->p + 1 < lx->end && !(lx->p[0] == '*' && lx->p[1] == '/'))
                                lex_next(lx);
                        if (lx->p + 1 < lx->end)
                                lex_next(lx), lex_next(lx);
                        continue;
                }
                return;
        }
}

static bool is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_ident_char(char c)  { return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'; }

static void lex_token(Lex *lx)
{
        lex_ws(lx);
        Token *t = &lx->cur;
        t->line = lx->line;
        t->col_pos = lx->col;
        t->unit = 0;
        t->s = NULL;

        if (lx->p >= lx->end) {
                t->kind = TK_EOF;
                return;
        }
        char c = lex_peek(lx);

        if (is_ident_start(c)) {
                const char *st = lx->p;
                while (lx->p < lx->end && is_ident_char(lex_peek(lx)))
                        lex_next(lx);
                t->kind = TK_IDENT;
                t->s = arena_strndup(lx->ar, st, (size_t)(lx->p - st));
                return;
        }

        /* A number may carry a leading '-' (docs promise `-3`); the sign is
         * scanned into the same token span so rev_parse_f64 sees it. */
        if (isdigit((unsigned char)c)
            || (c == '-' && lx->p + 1 < lx->end && isdigit((unsigned char)lx->p[1]))
            || (c == '.' && lx->p + 1 < lx->end && isdigit((unsigned char)lx->p[1]))) {
                const char *st = lx->p;
                if (c == '-')
                        lex_next(lx);
                while (lx->p < lx->end && isdigit((unsigned char)lex_peek(lx)))
                        lex_next(lx);
                if (lex_peek(lx) == '.') {
                        lex_next(lx);
                        while (lx->p < lx->end && isdigit((unsigned char)lex_peek(lx)))
                                lex_next(lx);
                }
                if (lex_peek(lx) == 'e' || lex_peek(lx) == 'E') {
                        const char *save = lx->p;
                        lex_next(lx);
                        if (lex_peek(lx) == '+' || lex_peek(lx) == '-')
                                lex_next(lx);
                        if (isdigit((unsigned char)lex_peek(lx))) {
                                while (lx->p < lx->end && isdigit((unsigned char)lex_peek(lx)))
                                        lex_next(lx);
                        } else {
                                lx->p = save;   /* not an exponent */
                        }
                }
                t->kind = TK_NUM;
                const char *pos = NULL;
                if (!rev_parse_f64(st, (size_t)(lx->p - st), &pos, &t->num)) {
                        lex_fail(lx, "line %d: bad number", t->line);
                        return;
                }
                /* unit suffix */
                char u = lex_peek(lx);
                if (u == '%') {
                        t->unit = '%';
                        lex_next(lx);
                } else if (u == 's' || u == 'S') {
                        t->unit = 's';
                        lex_next(lx);
                } else if ((u == 'm' || u == 'M') && lx->p + 1 < lx->end &&
                           (lx->p[1] == 's' || lx->p[1] == 'S')) {
                        t->unit = 'm';
                        lex_next(lx);
                        lex_next(lx);
                }
                return;
        }

        if (c == '"') {
                lex_next(lx);
                size_t cap = 64;
                char *buf = (char *)arena_alloc(lx->ar, cap);
                size_t o = 0;
                while (lx->p < lx->end && lex_peek(lx) != '"') {
                        char ch = lex_next(lx);
                        if (ch == '\\' && lx->p < lx->end) {
                                char e = lex_next(lx);
                                if (e == 'n') ch = '\n';
                                else if (e == 't') ch = '\t';
                                else ch = e;
                        }
                        if (o + 1 >= cap) {
                                char *nb = (char *)arena_alloc(lx->ar, cap * 2);
                                memcpy(nb, buf, o);
                                buf = nb;
                                cap *= 2;
                        }
                        buf[o++] = ch;
                }
                if (lex_peek(lx) != '"') {
                        lex_fail(lx, "line %d: unterminated string", t->line);
                        return;
                }
                lex_next(lx);
                buf[o] = '\0';
                t->kind = TK_STR;
                t->s = buf;
                return;
        }

        if (c == '#') {
                const char *st = lx->p;
                size_t maxlen = 0;
                while (st + maxlen < lx->end && maxlen < 10 &&
                       (isxdigit((unsigned char)st[maxlen]) || (maxlen == 0 && st[0] == '#')))
                        maxlen++;
                if (!rev_parse_hex_color(st, maxlen, &t->col)) {
                        lex_fail(lx, "line %d: bad color at '#'", t->line);
                        return;
                }
                for (size_t i = 0; i < maxlen; i++)
                        lex_next(lx);
                t->kind = TK_COLOR;
                return;
        }

        switch (c) {
        case '{': lex_next(lx); t->kind = TK_LBRACE;   return;
        case '}': lex_next(lx); t->kind = TK_RBRACE;   return;
        case '(': lex_next(lx); t->kind = TK_LPAREN;   return;
        case ')': lex_next(lx); t->kind = TK_RPAREN;   return;
        case '[': lex_next(lx); t->kind = TK_LBRACKET; return;
        case ']': lex_next(lx); t->kind = TK_RBRACKET; return;
        case ',': lex_next(lx); t->kind = TK_COMMA;    return;
        case ';': lex_next(lx); t->kind = TK_SEMI;     return;
        case '=': lex_next(lx); t->kind = TK_EQ;       return;
        default:
                lex_fail(lx, "line %d: unexpected character '%c' (0x%02x)", t->line, isprint((unsigned char)c) ? c : '?', (unsigned)c);
                return;
        }
}

/* --------------------------------------------------------------------------
 * parser
 * ------------------------------------------------------------------------ */

typedef struct {
        Lex lx;
        RArena *ar;
        bool err;
        char errmsg[192];
} Parser;

static bool p_fail(Parser *pp, const char *fmt, ...)
{
        if (!pp->err) {
                va_list ap;
                va_start(ap, fmt);
                vsnprintf(pp->errmsg, sizeof pp->errmsg, fmt, ap);
                va_end(ap);
                pp->err = true;
        }
        return false;
}

static bool p_at(Parser *pp, TokKind k)
{
        return !pp->err && pp->lx.cur.kind == k;
}

static bool p_accept(Parser *pp, TokKind k)
{
        if (p_at(pp, k)) {
                lex_token(&pp->lx);
                return true;
        }
        return false;
}

static bool p_expect(Parser *pp, TokKind k, const char *what)
{
        if (!p_at(pp, k)) {
                return p_fail(pp, "line %d: expected %s, got %s",
                              pp->lx.cur.line, what,
                              pp->lx.cur.kind == TK_EOF ? "end of file"
                              : pp->lx.cur.kind == TK_IDENT || pp->lx.cur.kind == TK_STR ? pp->lx.cur.s
                              : "symbol");
        }
        lex_token(&pp->lx);
        return true;
}

static bool tok_starts_value(TokKind k)
{
        return k == TK_NUM || k == TK_STR || k == TK_COLOR || k == TK_IDENT ||
               k == TK_LPAREN || k == TK_LBRACKET;
}

static RVal *p_value(Parser *pp);

static RVal *new_val(Parser *pp, u8 kind)
{
        RVal *v = (RVal *)arena_zalloc(pp->ar, sizeof(RVal));
        v->kind = kind;
        return v;
}

static RVal *p_value(Parser *pp)
{
        if (pp->err)
                return NULL;
        Token t = pp->lx.cur;

        switch (t.kind) {
        case TK_NUM: {
                RVal *v = new_val(pp, RV_NUM);
                v->num = t.num;
                v->str = (char *)(uintptr_t)t.unit;   /* unit parked on str */
                lex_token(&pp->lx);
                return v;
        }
        case TK_STR: {
                RVal *v = new_val(pp, RV_STR);
                v->str = t.s;
                lex_token(&pp->lx);
                return v;
        }
        case TK_COLOR: {
                RVal *v = new_val(pp, RV_COLOR);
                v->color = t.col;
                lex_token(&pp->lx);
                return v;
        }
        case TK_IDENT: {
                RVal *v = new_val(pp, RV_IDENT);
                v->str = t.s;
                if (strcmp(t.s, "true") == 0)  { v->kind = RV_BOOL; v->b = true; }
                if (strcmp(t.s, "false") == 0) { v->kind = RV_BOOL; v->b = false; }
                lex_token(&pp->lx);
                return v;
        }
        case TK_LPAREN: {
                lex_token(&pp->lx);
                RVal *v = new_val(pp, RV_TUPLE);
                int cap = 4;
                v->items = (RVal **)arena_alloc(pp->ar, (size_t)cap * sizeof(RVal *));
                for (;;) {
                        RVal *it = p_value(pp);
                        if (!it)
                                return NULL;
                        if (v->n == cap) {
                                RVal **ni = (RVal **)arena_alloc(pp->ar, (size_t)cap * 2 * sizeof(RVal *));
                                memcpy(ni, v->items, (size_t)v->n * sizeof(RVal *));
                                v->items = ni;
                                cap *= 2;
                        }
                        v->items[v->n++] = it;
                        /* separators are optional inside tuples */
                        if (!p_accept(pp, TK_COMMA) && !tok_starts_value(pp->lx.cur.kind))
                                break;
                }
                if (!p_expect(pp, TK_RPAREN, "')'"))
                        return NULL;
                return v;
        }
        case TK_LBRACKET: {
                lex_token(&pp->lx);
                RVal *v = new_val(pp, RV_LIST);
                int cap = 4;
                v->items = (RVal **)arena_alloc(pp->ar, (size_t)cap * sizeof(RVal *));
                if (p_at(pp, TK_RBRACKET)) {
                        lex_token(&pp->lx);
                        return v;
                }
                for (;;) {
                        RVal *it = p_value(pp);
                        if (!it)
                                return NULL;
                        if (v->n == cap) {
                                RVal **ni = (RVal **)arena_alloc(pp->ar, (size_t)cap * 2 * sizeof(RVal *));
                                memcpy(ni, v->items, (size_t)v->n * sizeof(RVal *));
                                v->items = ni;
                                cap *= 2;
                        }
                        v->items[v->n++] = it;
                        /* separators are optional inside lists too */
                        if (!p_accept(pp, TK_COMMA) && !tok_starts_value(pp->lx.cur.kind))
                                break;
                }
                if (!p_expect(pp, TK_RBRACKET, "']'"))
                        return NULL;
                return v;
        }
        default:
                p_fail(pp, "line %d: expected a value, got %s",
                       t.line, t.kind == TK_EOF ? "end of file"
                       : (t.kind == TK_IDENT || t.kind == TK_STR) ? t.s : "symbol");
                return NULL;
        }
}

static bool push_prop(Parser *pp, RProp **props, int *n, const char *key, RVal *val, int line, int col)
{
        /* duplicate keys: last one wins (replace in place) */
        for (int i = 0; i < *n; i++) {
                if (strcmp((*props)[i].key, key) == 0) {
                        (*props)[i].val = val;
                        (*props)[i].line = line;
                        return true;
                }
        }
        if (*n % 8 == 0) {
                RProp *np = (RProp *)arena_alloc(pp->ar, (size_t)(*n + 8) * sizeof(RProp));
                if (*n)
                        memcpy(np, *props, (size_t)*n * sizeof(RProp));
                *props = np;
        }
        (*props)[*n].key = (char *)key;
        (*props)[*n].val = val;
        (*props)[*n].line = line;
        (*props)[*n].col = col;
        (*n)++;
        return true;
}

static bool parse_body(Parser *pp, RProp **props, int *nprops,
                       RAnim **anims, int *nanims, RLayerIR ***layers, int *nlayers);

static bool parse_body(Parser *pp, RProp **props, int *nprops,
                       RAnim **anims, int *nanims, RLayerIR ***layers, int *nlayers)
{
        if (!p_expect(pp, TK_LBRACE, "'{'"))
                return false;

        for (;;) {
                if (p_at(pp, TK_RBRACE)) {
                        lex_token(&pp->lx);
                        return true;
                }
                if (p_at(pp, TK_EOF))
                        return p_fail(pp, "line %d: unexpected end of file in block", pp->lx.cur.line);

                if (p_at(pp, TK_IDENT)) {
                        Token key = pp->lx.cur;
                        lex_token(&pp->lx);

                        if (strcmp(key.s, "layer") == 0 && layers) {
                                if (!p_at(pp, TK_IDENT))
                                        return p_fail(pp, "line %d: 'layer' needs a name", key.line);
                                Token name = pp->lx.cur;
                                lex_token(&pp->lx);

                                RLayerIR *L = (RLayerIR *)arena_zalloc(pp->ar, sizeof(RLayerIR));
                                L->name = name.s;
                                L->line = key.line;
                                if (!parse_body(pp, &L->props, &L->nprops, &L->anims, &L->nanims, NULL, NULL))
                                        return false;
                                if (*nlayers % 8 == 0) {
                                        RLayerIR **nl = (RLayerIR **)arena_alloc(pp->ar, (size_t)(*nlayers + 8) * sizeof(RLayerIR *));
                                        if (*nlayers)
                                                memcpy(nl, *layers, (size_t)*nlayers * sizeof(RLayerIR *));
                                        *layers = nl;
                                }
                                (*layers)[(*nlayers)++] = L;
                                continue;
                        }

                        if (strcmp(key.s, "anim") == 0 && anims) {
                                if (!p_at(pp, TK_IDENT))
                                        return p_fail(pp, "line %d: 'anim' needs a target property name", key.line);
                                Token target = pp->lx.cur;
                                lex_token(&pp->lx);

                                RAnim *a = (RAnim *)arena_zalloc(pp->ar, sizeof(RAnim));
                                a->target = target.s;
                                a->line = key.line;
                                if (!parse_body(pp, &a->props, &a->nprops, NULL, NULL, NULL, NULL))
                                        return false;
                                if (*nanims % 8 == 0) {
                                        RAnim *na = (RAnim *)arena_alloc(pp->ar, (size_t)(*nanims + 8) * sizeof(RAnim));
                                        if (*nanims)
                                                memcpy(na, *anims, (size_t)*nanims * sizeof(RAnim));
                                        *anims = na;
                                }
                                (*anims)[(*nanims)++] = *a;
                                continue;
                        }

                        if (!p_expect(pp, TK_EQ, "'=' after property name"))
                                return false;
                        RVal *val = p_value(pp);
                        if (!val)
                                return false;
                        p_accept(pp, TK_SEMI);
                        if (!push_prop(pp, props, nprops, key.s, val, key.line, key.col_pos))
                                return false;
                        continue;
                }

                return p_fail(pp, "line %d: expected a property or 'layer'", pp->lx.cur.line);
        }
}

RSceneIR *rev_parse_scene(const char *text, size_t len, RArena **arena_out,
                          char *err, size_t errcap)
{
        RArena *ar = arena_create(16 * 1024);
        Parser pp;
        memset(&pp, 0, sizeof pp);
        pp.ar = ar;
        pp.lx.p = text;
        pp.lx.end = text + len;
        pp.lx.line = 1;
        pp.lx.col = 1;
        pp.lx.ar = ar;

        lex_token(&pp.lx);

        /* optional leading 'scene' keyword */
        if (p_at(&pp, TK_IDENT) && strcmp(pp.lx.cur.s, "scene") == 0) {
                lex_token(&pp.lx);
                if (p_at(&pp, TK_STR))
                        lex_token(&pp.lx);   /* optional title (kept in IR below) */
        } else {
                /* allow bare block: rewind by re-parsing from start */
                pp.lx.p = text;
                pp.lx.end = text + len;
                pp.lx.line = 1;
                pp.lx.col = 1;
                lex_token(&pp.lx);
        }

        RSceneIR *sc = (RSceneIR *)arena_zalloc(ar, sizeof(RSceneIR));
        sc->line = pp.lx.cur.line;

        if (!parse_body(&pp, &sc->props, &sc->nprops, NULL, NULL, &sc->layers, &sc->nlayers))
                goto fail;

        if (!p_at(&pp, TK_EOF)) {
                p_fail(&pp, "line %d: trailing content after scene block", pp.lx.cur.line);
                goto fail;
        }

        *arena_out = ar;
        return sc;

fail:
        if (err && errcap) {
                if (pp.err)
                        snprintf(err, errcap, "%s", pp.errmsg);
                else if (pp.lx.err)
                        snprintf(err, errcap, "%s", pp.lx.errmsg);
                else
                        snprintf(err, errcap, "parse error");
        }
        arena_destroy(ar);
        return NULL;
}

/* --------------------------------------------------------------------------
 * dump
 * ------------------------------------------------------------------------ */

static void dump_val(const RVal *v, int indent)
{
        switch (v->kind) {
        case RV_NUM:   printf("%g", v->num); break;
        case RV_STR:   printf("\"%s\"", v->str); break;
        case RV_IDENT: printf("%s", v->str); break;
        case RV_BOOL:  printf("%s", v->b ? "true" : "false"); break;
        case RV_COLOR: printf("#%02x%02x%02x%02x", v->color.r, v->color.g, v->color.b, v->color.a); break;
        case RV_TUPLE:
                printf("(");
                for (int i = 0; i < v->n; i++) { if (i) printf(", "); dump_val(v->items[i], indent); }
                printf(")");
                break;
        case RV_LIST:
                printf("[");
                for (int i = 0; i < v->n; i++) { if (i) printf(", "); dump_val(v->items[i], indent); }
                printf("]");
                break;
        }
}

void rev_dump_scene(const RSceneIR *sc)
{
        printf("scene {\n");
        for (int i = 0; i < sc->nprops; i++) {
                printf("  %s = ", sc->props[i].key);
                dump_val(sc->props[i].val, 0);
                printf("    ;  // line %d\n", sc->props[i].line);
        }
        for (int i = 0; i < sc->nlayers; i++) {
                const RLayerIR *L = sc->layers[i];
                printf("  layer %s {   // line %d\n", L->name, L->line);
                for (int k = 0; k < L->nprops; k++) {
                        printf("    %s = ", L->props[k].key);
                        dump_val(L->props[k].val, 0);
                        printf("    ;  // line %d\n", L->props[k].line);
                }
                for (int k = 0; k < L->nanims; k++) {
                        const RAnim *a = &L->anims[k];
                        printf("    anim %s {   // line %d\n", a->target, a->line);
                        for (int q = 0; q < a->nprops; q++) {
                                printf("      %s = ", a->props[q].key);
                                dump_val(a->props[q].val, 0);
                                printf("\n");
                        }
                        printf("    }\n");
                }
                printf("  }\n");
        }
        printf("}\n");
}
