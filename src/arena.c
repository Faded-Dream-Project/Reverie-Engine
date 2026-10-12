/* ============================================================================
 * Reverie Engine — arena.c
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RBlock {
	struct RBlock *next;
	size_t         used;   /* bytes used including the header */
	size_t         cap;    /* total block size including the header */
} RBlock;

struct RArena {
	RBlock *head;
	size_t  total;
};

#define ARENA_ALIGN   16u
#define BLOCK_HEADER  sizeof(RBlock)
#define MIN_BLOCK     (64u * 1024u)

static RBlock *block_new(size_t cap_with_header)
{
	RBlock *b = (RBlock *)malloc(cap_with_header);
	if (!b) {
		fprintf(stderr, "reverie: fatal: out of memory (arena block of %zu bytes)\n",
		        cap_with_header);
		abort();
	}
	b->next = NULL;
	b->used = BLOCK_HEADER;
	b->cap  = cap_with_header;
	return b;
}

RArena *arena_create(size_t initial_capacity)
{
	if (initial_capacity < MIN_BLOCK)
		initial_capacity = MIN_BLOCK;
	RArena *a = (RArena *)calloc(1, sizeof(RArena));
	if (!a) {
		fprintf(stderr, "reverie: fatal: out of memory (arena header)\n");
		abort();
	}
	a->head  = block_new(initial_capacity);
	a->total = 0;
	return a;
}

void arena_destroy(RArena *a)
{
	if (!a) return;
	RBlock *b = a->head;
	while (b) {
		RBlock *n = b->next;
		free(b);
		b = n;
	}
	free(a);
}

void *arena_alloc(RArena *a, size_t n)
{
	if (n == 0) n = 1;
	n = (n + (ARENA_ALIGN - 1)) & ~((size_t)ARENA_ALIGN - 1);

	RBlock *b = a->head;
	if (b->used + n > b->cap) {
		/* New block: geometric growth, but never smaller than the request. */
		size_t want = b->cap * 2;
		if (want < BLOCK_HEADER + n)
			want = BLOCK_HEADER + n;
		RBlock *nb = block_new(want);
		nb->next = a->head;
		a->head = nb;
		b = nb;
	}

	void *p = (u8 *)b + b->used;
	b->used += n;
	a->total += n;
	return p;
}

void *arena_zalloc(RArena *a, size_t n)
{
	void *p = arena_alloc(a, n);
	memset(p, 0, n);
	return p;
}

char *arena_strdup(RArena *a, const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = (char *)arena_alloc(a, n);
	memcpy(p, s, n);
	return p;
}

char *arena_strndup(RArena *a, const char *s, size_t len)
{
	char *p = (char *)arena_alloc(a, len + 1);
	memcpy(p, s, len);
	p[len] = '\0';
	return p;
}

size_t arena_total_used(const RArena *a)
{
	return a ? a->total : 0;
}
