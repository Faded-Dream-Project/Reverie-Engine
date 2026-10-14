/* ============================================================================
 * Reverie Engine — json.h
 * Minimal JSON reader for theme.json: objects, arrays, strings (with the
 * standard escapes), numbers, true/false/null. Arena-allocated parse tree;
 * strict about trailing garbage. Enough for theme metadata, nothing more —
 * by design.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_JSON_H
#define REV_JSON_H

#include "arena.h"
#include "rev_types.h"

enum {
        RJ_NULL = 0,
        RJ_BOOL,
        RJ_NUM,
        RJ_STR,
        RJ_ARR,
        RJ_OBJ
};

typedef struct RJVal RJVal;
struct RJVal {
        u8 kind;
        bool b;
        f64 num;
        char *str;            /* RJ_STR                            */
        char **keys;          /* RJ_OBJ (n entries)               */
        RJVal **items;        /* RJ_OBJ values / RJ_ARR elements  */
        int n;
};

/* Parses `text`; on success returns the root (object or array) and gives
 * you its arena. On failure returns NULL with a message + offset in err. */
RJVal *rev_json_parse(const char *text, size_t len, RArena **arena_out,
                      char *err, size_t errcap);

/* Object member lookup; NULL when absent or not an object. */
const RJVal *rev_json_get(const RJVal *obj, const char *key);

/* Typed accessors; the _def variants return `def` on type mismatch. */
const char *rev_json_str(const RJVal *v, const char *def);
f64 rev_json_num(const RJVal *v, f64 def);
bool rev_json_bool(const RJVal *v, bool def);
int rev_json_len(const RJVal *v);

#endif /* REV_JSON_H */
