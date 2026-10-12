/* ============================================================================
 * Reverie Engine — rev.h
 * The .rev scene language: lexer, recursive-descent parser, typed IR.
 *
 * Grammar (see docs/SCENE-FORMAT.md):
 *   scene  := "scene" [STRING] "{" (layer | prop)* "}"
 *   layer  := "layer" IDENT "{" (prop | anim)* "}"
 *   anim   := "anim" IDENT "{" prop* "}"
 *   prop   := IDENT "=" value [";"]
 *   value  := number[unit] | string | color | ident | bool
 *           | "(" value ("," value)* ")"        tuple
 *           | "[" value ("," value)* "]"        list
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_LANG_H
#define REV_LANG_H

#include "arena.h"
#include "rev_types.h"

typedef enum {
	RV_NUM = 0,
	RV_STR,
	RV_COLOR,
	RV_IDENT,
	RV_BOOL,
	RV_TUPLE,
	RV_LIST
} RValKind;

typedef struct RVal RVal;
struct RVal {
	u8      kind;
	f64     num;        /* RV_NUM                          */
	char   *str;        /* RV_STR / RV_IDENT               */
	RColor  color;      /* RV_COLOR                        */
	bool    b;          /* RV_BOOL                         */
	RVal  **items;      /* RV_TUPLE / RV_LIST              */
	int     n;
};

typedef struct {
	char *key;
	RVal *val;
	int   line, col;
} RProp;

typedef struct {
	char  *target;      /* property name this anim drives  */
	RProp *props;
	int    nprops;
	int    line;
} RAnim;

typedef struct {
	char   *name;
	RProp  *props;
	int     nprops;
	RAnim  *anims;
	int     nanims;
	int     line;
} RLayerIR;

typedef struct {
	char    *title;
	RProp   *props;
	int      nprops;
	RLayerIR **layers;
	int      nlayers;
	int      line;
} RSceneIR;

/* Parses text; on success returns the IR and gives you its arena (destroy
 * with arena_destroy when done). On failure returns NULL with a message in
 * `err` including line/column. */
RSceneIR *rev_parse_scene(const char *text, size_t len, RArena **arena_out,
                          char *err, size_t errcap);

/* Debug dump (revtool info). */
void rev_dump_scene(const RSceneIR *sc);

#endif /* REV_LANG_H */
