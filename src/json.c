/* ============================================================================
 * Reverie Engine — json.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
        const char *s;
        size_t len;
        size_t pos;
        char *err;
        size_t errcap;
        bool failed;
} JP;

static void jerr(JP *p, const char *msg)
{
        if (!p->failed && p->errcap) {
                snprintf(p->err, p->errcap, "json: %s at offset %zu", msg, p->pos);
                p->failed = true;
        }
}

static void jskip(JP *p)
{
        while (p->pos < p->len) {
                char c = p->s[p->pos];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        p->pos++;
                else
                        break;
        }
}

static int jpeek(JP *p)
{
        return p->pos < p->len ? (unsigned char)p->s[p->pos] : -1;
}

static RJVal *jnew(RArena *ar, int kind)
{
        RJVal *v = (RJVal *)arena_alloc(ar, sizeof(RJVal));
        if (v)
                memset(v, 0, sizeof *v);
        v->kind = (u8)kind;
        return v;
}

static RJVal *jvalue(JP *p, RArena *ar);

static bool jstring(JP *p, RArena *ar, char **out)
{
        *out = NULL;
        if (jpeek(p) != '"') {
                jerr(p, "expected string");
                return false;
        }
        p->pos++;
        char *buf = (char *)arena_alloc(ar, p->len - p->pos + 1);
        if (!buf)
                return false;
        size_t n = 0;
        while (p->pos < p->len) {
                unsigned char c = (unsigned char)p->s[p->pos];
                if (c == '"') {
                        p->pos++;
                        buf[n] = 0;
                        *out = buf;
                        return true;
                }
                if (c == '\\') {
                        p->pos++;
                        if (p->pos >= p->len)
                                break;
                        char e = p->s[p->pos];
                        switch (e) {
                        case '"': buf[n++] = '"'; break;
                        case '\\': buf[n++] = '\\'; break;
                        case '/': buf[n++] = '/'; break;
                        case 'b': buf[n++] = '\b'; break;
                        case 'f': buf[n++] = '\f'; break;
                        case 'n': buf[n++] = '\n'; break;
                        case 'r': buf[n++] = '\r'; break;
                        case 't': buf[n++] = '\t'; break;
                        case 'u': {
                                unsigned cp = 0;
                                if (p->pos + 4 >= p->len) {
                                        jerr(p, "bad \\u escape");
                                        return false;
                                }
                                for (int i = 0; i < 4; i++) {
                                        char h = p->s[p->pos + 1 + i];
                                        cp <<= 4;
                                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                                        else { jerr(p, "bad \\u escape"); return false; }
                                }
                                p->pos += 4;
                                /* UTF-8 encode (BMP only; no surrogate pairs —
                                 * theme files do not need them) */
                                if (cp < 0x80) {
                                        buf[n++] = (char)cp;
                                } else if (cp < 0x800) {
                                        buf[n++] = (char)(0xC0 | (cp >> 6));
                                        buf[n++] = (char)(0x80 | (cp & 0x3F));
                                } else {
                                        buf[n++] = (char)(0xE0 | (cp >> 12));
                                        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                        buf[n++] = (char)(0x80 | (cp & 0x3F));
                                }
                                break;
                        }
                        default:
                                jerr(p, "bad escape");
                                return false;
                        }
                        p->pos++;
                        continue;
                }
                if (c < 0x20) {
                        jerr(p, "control char in string");
                        return false;
                }
                buf[n++] = (char)c;
                p->pos++;
        }
        jerr(p, "unterminated string");
        return false;
}

static RJVal *jvalue(JP *p, RArena *ar)
{
        jskip(p);
        int c = jpeek(p);
        if (c == -1) {
                jerr(p, "unexpected end");
                return NULL;
        }

        if (c == '{') {
                p->pos++;
                RJVal *v = jnew(ar, RJ_OBJ);
                int cap = 0, n = 0;
                jskip(p);
                if (jpeek(p) == '}') {
                        p->pos++;
                        return v;
                }
                for (;;) {
                        jskip(p);
                        char *key = NULL;
                        if (!jstring(p, ar, &key))
                                return NULL;
                        jskip(p);
                        if (jpeek(p) != ':') {
                                jerr(p, "expected ':'");
                                return NULL;
                        }
                        p->pos++;
                        RJVal *val = jvalue(p, ar);
                        if (!val)
                                return NULL;
                        if (n == cap) {
                                int ncap = cap ? cap * 2 : 8;
                                char **nk = (char **)arena_alloc(ar,
                                                (size_t)ncap * sizeof(char *));
                                RJVal **ni = (RJVal **)arena_alloc(ar,
                                                (size_t)ncap * sizeof(RJVal *));
                                if (!nk || !ni)
                                        return NULL;
                                if (cap) {
                                        memcpy(nk, v->keys,
                                               (size_t)cap * sizeof(char *));
                                        memcpy(ni, v->items,
                                               (size_t)cap * sizeof(RJVal *));
                                }
                                v->keys = nk;
                                v->items = ni;
                                cap = ncap;
                        }
                        v->keys[n] = key;
                        v->items[n] = val;
                        n++;
                        v->n = n;
                        jskip(p);
                        if (jpeek(p) == ',') {
                                p->pos++;
                                continue;
                        }
                        if (jpeek(p) == '}') {
                                p->pos++;
                                return v;
                        }
                        jerr(p, "expected ',' or '}'");
                        return NULL;
                }
        }

        if (c == '[') {
                p->pos++;
                RJVal *v = jnew(ar, RJ_ARR);
                int cap = 0, n = 0;
                jskip(p);
                if (jpeek(p) == ']') {
                        p->pos++;
                        return v;
                }
                for (;;) {
                        RJVal *val = jvalue(p, ar);
                        if (!val)
                                return NULL;
                        if (n == cap) {
                                int ncap = cap ? cap * 2 : 8;
                                RJVal **ni = (RJVal **)arena_alloc(ar,
                                                (size_t)ncap * sizeof(RJVal *));
                                if (!ni)
                                        return NULL;
                                if (cap)
                                        memcpy(ni, v->items,
                                               (size_t)cap * sizeof(RJVal *));
                                v->items = ni;
                                cap = ncap;
                        }
                        v->items[n++] = val;
                        v->n = n;
                        jskip(p);
                        if (jpeek(p) == ',') {
                                p->pos++;
                                continue;
                        }
                        if (jpeek(p) == ']') {
                                p->pos++;
                                return v;
                        }
                        jerr(p, "expected ',' or ']'");
                        return NULL;
                }
        }

        if (c == '"') {
                char *s = NULL;
                if (!jstring(p, ar, &s))
                        return NULL;
                RJVal *v = jnew(ar, RJ_STR);
                v->str = s;
                return v;
        }

        if (c == 't' && p->pos + 4 <= p->len && memcmp(p->s + p->pos, "true", 4) == 0) {
                p->pos += 4;
                RJVal *v = jnew(ar, RJ_BOOL);
                v->b = true;
                return v;
        }
        if (c == 'f' && p->pos + 5 <= p->len && memcmp(p->s + p->pos, "false", 5) == 0) {
                p->pos += 5;
                RJVal *v = jnew(ar, RJ_BOOL);
                v->b = false;
                return v;
        }
        if (c == 'n' && p->pos + 4 <= p->len && memcmp(p->s + p->pos, "null", 4) == 0) {
                p->pos += 4;
                return jnew(ar, RJ_NULL);
        }

        /* number */
        {
                char buf[64];
                size_t start = p->pos;
                while (p->pos < p->len) {
                        char ch = p->s[p->pos];
                        if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' ||
                            ch == '.' || ch == 'e' || ch == 'E')
                                p->pos++;
                        else
                                break;
                }
                size_t nlen = p->pos - start;
                if (nlen == 0 || nlen >= sizeof buf) {
                        jerr(p, "bad value");
                        return NULL;
                }
                memcpy(buf, p->s + start, nlen);
                buf[nlen] = 0;
                char *end = NULL;
                double d = strtod(buf, &end);
                if (!end || *end) {
                        jerr(p, "bad number");
                        return NULL;
                }
                RJVal *v = jnew(ar, RJ_NUM);
                v->num = d;
                return v;
        }
}

RJVal *rev_json_parse(const char *text, size_t len, RArena **arena_out,
                      char *err, size_t errcap)
{
        if (err && errcap)
                err[0] = 0;
        if (!text || len == 0) {
                if (err && errcap)
                        snprintf(err, errcap, "json: empty input");
                return NULL;
        }
        RArena *ar = arena_create(len / 4 + 4096);
        if (!ar) {
                if (err && errcap)
                        snprintf(err, errcap, "json: out of memory");
                return NULL;
        }
        JP p = { text, len, 0, err, errcap, false };
        RJVal *v = jvalue(&p, ar);
        if (v && !p.failed) {
                jskip(&p);
                if (p.pos != p.len) {
                        jerr(&p, "trailing garbage");
                        v = NULL;
                }
        }
        if (!v || p.failed) {
                arena_destroy(ar);
                if (err && errcap && err[0] == 0)
                        snprintf(err, errcap, "json: parse failed at offset %zu", p.pos);
                return NULL;
        }
        if (arena_out)
                *arena_out = ar;
        return v;
}

const RJVal *rev_json_get(const RJVal *obj, const char *key)
{
        if (!obj || obj->kind != RJ_OBJ || !key)
                return NULL;
        for (int i = 0; i < obj->n; i++)
                if (strcmp(obj->keys[i], key) == 0)
                        return obj->items[i];
        return NULL;
}

const char *rev_json_str(const RJVal *v, const char *def)
{
        return v && v->kind == RJ_STR && v->str ? v->str : def;
}

f64 rev_json_num(const RJVal *v, f64 def)
{
        return v && v->kind == RJ_NUM ? v->num : def;
}

bool rev_json_bool(const RJVal *v, bool def)
{
        return v && v->kind == RJ_BOOL ? v->b : def;
}

int rev_json_len(const RJVal *v)
{
        return v && (v->kind == RJ_ARR || v->kind == RJ_OBJ) ? v->n : 0;
}
