/* ============================================================================
 * Reverie Engine — rev_types.h
 * Common scalar types and tiny helpers shared by every module.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_TYPES_H
#define REV_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef float     f32;
typedef double    f64;

#define REV_VERSION "0.1.4"

/* Straight (non-premultiplied) 8-bit RGBA color. */
typedef struct { u8 r, g, b, a; } RColor;

typedef struct { i32 w, h; }           RSize;
typedef struct { i32 x, y, w, h; }     RRect;
typedef struct { f64 x, y; }           Vec2;

static inline RColor rev_rgba(u8 r, u8 g, u8 b, u8 a) {
        RColor c = { r, g, b, a }; return c;
}
static inline RColor rev_rgb(u8 r, u8 g, u8 b) {
        RColor c = { r, g, b, 255 }; return c;
}

static inline i32 rev_min_i32(i32 a, i32 b) { return a < b ? a : b; }
static inline i32 rev_max_i32(i32 a, i32 b) { return a > b ? a : b; }
static inline i32 rev_clamp_i32(i32 v, i32 lo, i32 hi) {
        return v < lo ? lo : (v > hi ? hi : v);
}
static inline f64 rev_clampf(f64 v, f64 lo, f64 hi) {
        return v < lo ? lo : (v > hi ? hi : v);
}
static inline f64 rev_lerpf(f64 a, f64 b, f64 t) { return a + (b - a) * t; }

static inline u32 rev_pack_rgba(RColor c) {
        return ((u32)c.a << 24) | ((u32)c.b << 16) | ((u32)c.g << 8) | (u32)c.r;
}

/* Linear interpolation between two colors (straight alpha). */
static inline RColor rev_lerp_color(RColor a, RColor b, f64 t) {
        RColor c;
        c.r = (u8)(a.r + (b.r - a.r) * t);
        c.g = (u8)(a.g + (b.g - a.g) * t);
        c.b = (u8)(a.b + (b.b - a.b) * t);
        c.a = (u8)(a.a + (b.a - a.a) * t);
        return c;
}

#endif /* REV_TYPES_H */
