/* ============================================================================
 * Reverie Engine — util.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ---- files ----------------------------------------------------------------*/

u8 *rev_read_file(const char *path, size_t *len)
{
        FILE *f = fopen(path, "rb");
        if (!f)
                return NULL;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
        long n = ftell(f);
        if (n < 0) { fclose(f); return NULL; }
        rewind(f);

        u8 *buf = (u8 *)malloc((size_t)n + 1);
        if (!buf) { fclose(f); return NULL; }

        size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        if (got != (size_t)n) {
                free(buf);
                return NULL;
        }
        buf[n] = 0;
        if (len)
                *len = (size_t)n;
        return buf;
}

bool rev_write_file(const char *path, const void *data, size_t len)
{
        FILE *f = fopen(path, "wb");
        if (!f)
                return false;
        bool ok = fwrite(data, 1, len, f) == len;
        if (fclose(f) != 0)
                ok = false;
        return ok;
}

bool rev_file_exists(const char *path)
{
        FILE *f = fopen(path, "rb");
        if (!f)
                return false;
        fclose(f);
        return true;
}

f64 rev_file_mtime(const char *path)
{
        struct stat st;
        if (stat(path, &st) != 0)
                return 0.0;
        return (f64)st.st_mtime;
}

/* ---- strings ---------------------------------------------------------------*/

char *rev_strncpy(char *dst, size_t cap, const char *src, size_t n)
{
        if (cap == 0)
                return dst;
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, k);
        dst[k] = '\0';
        return dst;
}

bool rev_str_ieq(const char *a, const char *b)
{
        return a && b && strcasecmp(a, b) == 0;
}

bool rev_str_ieq_n(const char *a, const char *b, size_t n)
{
        return a && b && strncasecmp(a, b, n) == 0;
}

bool rev_str_ends_with_ci(const char *s, const char *suffix)
{
        size_t ls = strlen(s), lx = strlen(suffix);
        if (lx > ls)
                return false;
        return strcasecmp(s + ls - lx, suffix) == 0;
}

char *rev_path_join(const char *dir, const char *name)
{
        if (!dir || !*dir || strcmp(dir, ".") == 0) {
                char *p = (char *)malloc(strlen(name) + 1);
                if (p) strcpy(p, name);
                return p;
        }
        size_t ld = strlen(dir);
        bool slash = ld > 0 && (dir[ld - 1] == '/');
        size_t total = ld + (slash ? 0 : 1) + strlen(name) + 1;
        char *p = (char *)malloc(total);
        if (!p) return NULL;
        snprintf(p, total, "%s%s%s", dir, slash ? "" : "/", name);
        return p;
}

char *rev_path_dirname(const char *path)
{
        const char *slash = strrchr(path, '/');
        if (!slash)
                return strdup(".");
        size_t n = (size_t)(slash - path);
        if (n == 0)
                n = 1; /* root "/" */
        char *p = (char *)malloc(n + 1);
        if (!p) return NULL;
        memcpy(p, path, n);
        p[n] = '\0';
        return p;
}

/* ---- numbers ---------------------------------------------------------------*/

bool rev_parse_f64(const char *s, size_t len, const char **pos, f64 *out)
{
        /* strtod needs NUL-termination; operate on a bounded local copy. */
        char tmp[64];
        if (len == 0 || len >= sizeof tmp)
                return false;
        memcpy(tmp, s, len);
        tmp[len] = '\0';

        char *end = NULL;
        errno = 0;
        f64 v = strtod(tmp, &end);
        if (end == tmp || errno == ERANGE)
                return false;
        *out = v;
        if (pos)
                *pos = s + (end - tmp);
        return true;
}

/* ---- colors ----------------------------------------------------------------*/

static int hexval(char c)
{
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
}

bool rev_parse_hex_color(const char *s, size_t maxlen, RColor *out)
{
        if (maxlen < 2 || s[0] != '#')
                return false;

        size_t rest = maxlen - 1;
        s++;
        int v[8];
        size_t n = 0;
        while (n < 8 && n < rest && hexval(s[n]) >= 0) {
                v[n] = hexval(s[n]);
                n++;
        }

        RColor c;
        switch (n) {
        case 3:
                c.r = (u8)(v[0] * 17); c.g = (u8)(v[1] * 17); c.b = (u8)(v[2] * 17);
                c.a = 255;
                break;
        case 4:
                c.r = (u8)(v[0] * 17); c.g = (u8)(v[1] * 17); c.b = (u8)(v[2] * 17);
                c.a = (u8)(v[3] * 17);
                break;
        case 6:
                c.r = (u8)(v[0] * 16 + v[1]); c.g = (u8)(v[2] * 16 + v[3]);
                c.b = (u8)(v[4] * 16 + v[5]); c.a = 255;
                break;
        case 8:
                c.r = (u8)(v[0] * 16 + v[1]); c.g = (u8)(v[2] * 16 + v[3]);
                c.b = (u8)(v[4] * 16 + v[5]); c.a = (u8)(v[6] * 16 + v[7]);
                break;
        default:
                return false;
        }
        *out = c;
        return true;
}

bool rev_parse_u8(const char *s, size_t len, u32 *out)
{
        if (len == 0 || len > 3)
                return false;
        u32 v = 0;
        for (size_t i = 0; i < len; i++) {
                if (s[i] < '0' || s[i] > '9')
                        return false;
                v = v * 10 + (u32)(s[i] - '0');
        }
        *out = v;
        return true;
}

/* ---- easing ----------------------------------------------------------------*/

static f64 ease_in_out(f64 t, f64 (*fin)(f64), f64 (*fout)(f64))
{
        return t < 0.5 ? 0.5 * fin(t * 2.0) : 0.5 + 0.5 * fout((t - 0.5) * 2.0);
}

static f64 in_sine(f64 t)    { return 1.0 - cos(t * M_PI / 2.0); }
static f64 out_sine(f64 t)   { return sin(t * M_PI / 2.0); }
static f64 in_quad(f64 t)    { return t * t; }
static f64 out_quad(f64 t)   { return 1.0 - (1.0 - t) * (1.0 - t); }
static f64 in_cubic(f64 t)   { return t * t * t; }
static f64 out_cubic(f64 t)  { f64 u = 1.0 - t; return 1.0 - u * u * u; }
static f64 in_quart(f64 t)   { return t * t * t * t; }
static f64 out_quart(f64 t)  { f64 u = 1.0 - t; return 1.0 - u * u * u * u; }
static f64 in_expo(f64 t)    { return t <= 0.0 ? 0.0 : pow(2.0, 10.0 * t - 10.0); }
static f64 out_expo(f64 t)   { return t >= 1.0 ? 1.0 : 1.0 - pow(2.0, -10.0 * t); }
static f64 in_back(f64 t)    { return t * t * (2.70158 * t - 1.70158); }
static f64 out_back(f64 t)   { f64 u = t - 1.0; return 1.0 + u * u * (2.70158 * u + 1.70158); }
static f64 in_elastic(f64 t)
{
        if (t <= 0.0) return 0.0;
        if (t >= 1.0) return 1.0;
        return -pow(2.0, 10.0 * t - 10.0) * sin((t * 10.0 - 10.75) * (2.0 * M_PI / 3.0));
}
static f64 out_elastic(f64 t)
{
        if (t <= 0.0) return 0.0;
        if (t >= 1.0) return 1.0;
        return pow(2.0, -10.0 * t) * sin((t * 10.0 - 0.75) * (2.0 * M_PI / 3.0)) + 1.0;
}

f64 rev_ease(int kind, f64 t)
{
        t = rev_clampf(t, 0.0, 1.0);
        switch (kind) {
        case REV_EASE_LINEAR:   return t;
        case REV_EASE_SINE:     return ease_in_out(t, in_sine, out_sine);
        case REV_EASE_QUAD:     return ease_in_out(t, in_quad, out_quad);
        case REV_EASE_CUBIC:    return ease_in_out(t, in_cubic, out_cubic);
        case REV_EASE_QUART:    return ease_in_out(t, in_quart, out_quart);
        case REV_EASE_EXPO:     return ease_in_out(t, in_expo, out_expo);
        case REV_EASE_BACK:     return ease_in_out(t, in_back, out_back);
        case REV_EASE_ELASTIC:  return ease_in_out(t, in_elastic, out_elastic);
        case REV_EASE_BOUNCE: {
                f64 d = 1.0;
                if (t < 4.0 / 11.0) return (121.0 * t * t) / 16.0;
                if (t < 8.0 / 11.0) { f64 u = t - 8.0 / 11.0; d -= (363.0 / 40.0) * u * u; return d; }
                if (t < 9.0 / 10.0) { f64 u = t - 9.0 / 10.0; d -= (99.0 / 10.0) * u * u;  return d; }
                f64 u = t - 21.0 / 22.0;
                return d - (4356.0 / 361.0) * u * u;
        }
        default: return t;
        }
}

/* ---- time ------------------------------------------------------------------*/

f64 rev_monotonic(void)
{
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
                return 0.0;
        return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

f64 rev_wallclock(void)
{
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
                return 0.0;
        return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}
