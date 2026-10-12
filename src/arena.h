/* ============================================================================
 * Reverie Engine — arena.h
 * Chained-block bump allocator. Scene IR (parsed .rev trees, SVG DOMs) lives
 * in arenas so a scene hot-reload is one arena_destroy() away — no per-node
 * free bookkeeping anywhere.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_ARENA_H
#define REV_ARENA_H

#include "rev_types.h"

typedef struct RArena RArena;

RArena *arena_create(size_t initial_capacity);
void    arena_destroy(RArena *a);

/* 16-byte aligned bump allocation; aborts on OOM (wallpaper scene graphs are
 * small and bounded; dying loudly beats limping). */
void   *arena_alloc(RArena *a, size_t n);
void   *arena_zalloc(RArena *a, size_t n);
char   *arena_strdup(RArena *a, const char *s);
/* Copy a (len, bytes) range and NUL-terminate it. */
char   *arena_strndup(RArena *a, const char *s, size_t len);

/* Total bytes currently handed out (all blocks, including headers). */
size_t  arena_total_used(const RArena *a);

#endif /* REV_ARENA_H */
