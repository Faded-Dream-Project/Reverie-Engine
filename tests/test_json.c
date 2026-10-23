/* ============================================================================
 * Reverie Engine — tests/test_json.c
 * Regression tests for the JSON parser (src/json.c), focused on the
 * container-growth bug found by the session record: objects and arrays
 * past the initial capacity of 8 used to lose their first entries — the
 * grown items/keys arrays were allocated fresh with the old contents
 * never copied, so items[0..7] read as uninitialized garbage. Every
 * theme.json in the tree has at most 8 keys, which is why nothing had
 * tripped it before a 10-token session record did.
 *
 * Also covers: values after the growth point, mixed types, nesting past
 * the threshold, and the typed accessors on grown containers.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/arena.h"
#include "../src/json.h"

static int failures;

#define CHECK(cond, msg)                                                \
        do {                                                            \
                if (cond) {                                             \
                        printf("  ok: %s\n", msg);                      \
                } else {                                                \
                        printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
                        failures++;                                     \
                }                                                        \
        } while (0)

static RJVal *parse(const char *s, RArena **ar)
{
        static char err[192];
        return rev_json_parse(s, strlen(s), ar, err, sizeof err);
}

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);
        RArena *ar = NULL;
        RJVal *root = NULL;

        /* ---- array growth: 10 string items past the cap-8 threshold ---- */
        {
                char json[512];
                snprintf(json, sizeof json,
                         "{\"argv\": [\"--scene\",\"/p\",\"--fps\",\"24\","
                         "\"--monitor\",\"0\",\"--renderer\",\"cpu\","
                         "\"--backend\",\"x11\"]}");
                root = parse(json, &ar);
                CHECK(root != NULL, "10-item array parses");
                const RJVal *arr = root ? rev_json_get(root, "argv") : NULL;
                CHECK(arr && arr->kind == RJ_ARR && rev_json_len(arr) == 10,
                      "grown array reports all 10 items");
                bool ok = arr != NULL;
                static const char *want[10] = { "--scene", "/p", "--fps", "24",
                                                "--monitor", "0", "--renderer",
                                                "cpu", "--backend", "x11" };
                for (int i = 0; ok && i < 10 && arr; i++)
                        ok = arr->items[i]->kind == RJ_STR &&
                             !strcmp(arr->items[i]->str, want[i]);
                CHECK(ok, "every grown-array item round-trips (the first 8 "
                          "used to be garbage)");
                if (ar) { arena_destroy(ar); ar = NULL; }
        }

        /* ---- object growth: 12 keys past the cap-8 threshold ----------- */
        {
                char json[768];
                snprintf(json, sizeof json,
                         "{\"k00\":0,\"k01\":1,\"k02\":2,\"k03\":3,"
                         "\"k04\":4,\"k05\":5,\"k06\":6,\"k07\":7,"
                         "\"k08\":8,\"k09\":9,\"k10\":10,\"k11\":11}");
                root = parse(json, &ar);
                CHECK(root != NULL && root->kind == RJ_OBJ &&
                      rev_json_len(root) == 12,
                      "grown object reports all 12 members");
                bool ok = root != NULL;
                for (int i = 0; ok && i < 12; i++) {
                        char key[16];
                        snprintf(key, sizeof key, "k%02d", i);
                        const RJVal *v = rev_json_get(root, key);
                        ok = v && rev_json_num(v, -1.0) == (f64)i;
                }
                CHECK(ok, "every grown-object member is reachable with its "
                          "own value (lookups used to hit garbage pointers)");
                if (ar) { arena_destroy(ar); ar = NULL; }
        }

        /* ---- mixed types across the threshold --------------------------- */
        {
                root = parse("{\"a\": [0, 1, 2, 3, 4, 5, 6, 7, true, "
                             "false, null, \"tail\"]}",
                             &ar);
                const RJVal *arr = root ? rev_json_get(root, "a") : NULL;
                CHECK(arr && rev_json_len(arr) == 12,
                      "mixed-type grown array parses");
                bool ok = arr != NULL;
                for (int i = 0; ok && i < 8 && arr; i++)
                        ok = arr->items[i]->kind == RJ_NUM &&
                             arr->items[i]->num == (f64)i;
                CHECK(ok, "numbers before the threshold survive the growth");
                CHECK(arr && arr->items[8]->kind == RJ_BOOL && arr->items[8]->b &&
                      arr && arr->items[9]->kind == RJ_BOOL && !arr->items[9]->b,
                      "booleans after the threshold parse");
                CHECK(arr && !strcmp(rev_json_str(arr->items[11], "?"), "tail"),
                      "string after the threshold parses");
                if (ar) { arena_destroy(ar); ar = NULL; }
        }

        /* ---- nesting past the threshold ---------------------------------- */
        {
                char json[640];
                snprintf(json, sizeof json,
                         "{\"outer\": [0, 1, 2, 3, 4, 5, 6, 7, "
                         "{\"inner\": [\"i0\",\"i1\",\"i2\",\"i3\",\"i4\","
                         "\"i5\",\"i6\",\"i7\",\"i8\"]}]}");
                root = parse(json, &ar);
                const RJVal *outer = root ? rev_json_get(root, "outer") : NULL;
                CHECK(outer && rev_json_len(outer) == 9,
                      "outer array grows past the threshold");
                const RJVal *inner = (outer && outer->items[8]->kind == RJ_OBJ)
                        ? rev_json_get(outer->items[8], "inner") : NULL;
                bool ok = inner != NULL && rev_json_len(inner) == 9;
                for (int i = 0; ok && i < 9; i++) {
                        char want[16];
                        snprintf(want, sizeof want, "i%d", i);
                        ok = inner->items[i]->kind == RJ_STR &&
                             !strcmp(inner->items[i]->str, want);
                }
                CHECK(ok, "a nested grown array inside a grown array keeps "
                          "all of its items");
                if (ar) { arena_destroy(ar); ar = NULL; }
        }

        printf(failures ? "json: %d FAILURE(S)\n" : "json: OK\n", failures);
        return failures ? 1 : 0;
}
