/* ============================================================================
 * Reverie Engine — util.h
 * File I/O, string helpers, color parsing, easing curves, monotonic time.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_UTIL_H
#define REV_UTIL_H

#include "rev_types.h"

/* ---- files ----------------------------------------------------------------*/
/* Reads a whole file (binary-safe). Returns malloc'd buffer of *len bytes,
 * NUL-terminated at [len] for the convenience of text parsers.
 * Returns NULL on failure. Caller owns the buffer (free()). */
u8   *rev_read_file(const char *path, size_t *len);
bool  rev_write_file(const char *path, const void *data, size_t len);
bool  rev_file_exists(const char *path);
/* File modification time in seconds since epoch; 0 on failure. */
f64   rev_file_mtime(const char *path);
/* mkdir -p (single path, no parents beyond '/'). Refuses paths longer
 * than REV_MKDIR_P_MAX bytes. EEXIST counts as success. */
#define REV_MKDIR_P_MAX 384
bool  rev_mkdir_p(const char *path);

/* ---- strings ---------------------------------------------------------------*/
/* Bounded, always-NUL-terminated copy. Returns dst. */
char *rev_strncpy(char *dst, size_t cap, const char *src, size_t n);
bool  rev_str_ieq(const char *a, const char *b);
bool  rev_str_ieq_n(const char *a, const char *b, size_t n);
bool  rev_str_ends_with_ci(const char *s, const char *suffix);
/* malloc'd "dir/name". Falls back to "name" when dir is empty/NULL. */
char *rev_path_join(const char *dir, const char *name);
/* Directory part of a path ("a/b/c.rev" -> "a/b"), malloc'd ("." if none). */
char *rev_path_dirname(const char *path);

/* ---- numbers ---------------------------------------------------------------*/
/* Strict parse of [s, s+len) as a double. Advances *pos past consumed chars. */
bool  rev_parse_f64(const char *s, size_t len, const char **pos, f64 *out);

/* ---- colors ----------------------------------------------------------------*/
/* Parses "#RGB", "#RGBA", "#RRGGBB", "#RRGGBBAA" (s starts AT '#'). */
bool  rev_parse_hex_color(const char *s, size_t maxlen, RColor *out);
bool  rev_parse_u8(const char *s, size_t len, u32 *out);

/* ---- easing ----------------------------------------------------------------*/
enum {
        REV_EASE_LINEAR = 0,
        REV_EASE_SINE,
        REV_EASE_QUAD,
        REV_EASE_CUBIC,
        REV_EASE_QUART,
        REV_EASE_EXPO,
        REV_EASE_BACK,
        REV_EASE_ELASTIC,
        REV_EASE_BOUNCE,
        REV_EASE_COUNT
};
/* Maps t in [0,1] through an in-out variant of the curve. */
f64   rev_ease(int kind, f64 t);

/* ---- time ------------------------------------------------------------------*/
f64   rev_monotonic(void);   /* seconds */
f64   rev_wallclock(void);   /* seconds since epoch */

#endif /* REV_UTIL_H */
