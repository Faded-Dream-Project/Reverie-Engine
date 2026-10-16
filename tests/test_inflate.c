/* ============================================================================
 * Reverie Engine — tests/test_inflate.c
 * Verifies the DEFLATE inflater against streams produced by independent
 * implementations (zlib at level 0/9, Z_FIXED strategy, raw wrapper).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inflate.h"
#include "util.h"

static int g_fails = 0;

#define CHECK(cond, ...)                                                    \
        do {                                                                    \
                if (!(cond)) {                                                      \
                        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
                        printf(__VA_ARGS__);                                            \
                        printf("\n");                                                   \
                        g_fails++;                                                      \
                }                                                                   \
        } while (0)

static u8 *slurp(const char *path, size_t *len)
{
        u8 *d = rev_read_file(path, len);
        if (!d) {
                printf("  FATAL cannot read fixture %s\n", path);
                exit(2);
        }
        return d;
}

static void test_pair(const char *zpath, const char *rpath, bool raw)
{
        size_t zlen = 0, rlen = 0;
        u8 *z = slurp(zpath, &zlen);
        u8 *r = slurp(rpath, &rlen);

        u8 *out = NULL;
        size_t outlen = 0;
        int rc = raw ? rev_inflate_raw(z, zlen, 0, &out, &outlen)
                     : rev_inflate_zlib(z, zlen, 0, &out, &outlen);
        CHECK(rc == REV_INF_OK, "%s: inflate rc=%d (%s)", zpath, rc,
              rev_inflate_strerror(rc));
        if (rc == REV_INF_OK) {
                CHECK(outlen == rlen, "%s: length %zu != %zu", zpath, outlen, rlen);
                if (outlen == rlen)
                        CHECK(memcmp(out, r, rlen) == 0, "%s: content mismatch", zpath);
        }
        free(out);
        free(z);
        free(r);
}

int main(void)
{
        const char *names[] = { "text", "random", "matches", "mixed" };

        printf("test_inflate:\n");

        for (size_t i = 0; i < 4; i++) {
                char zpath[512], rpath[512];

                snprintf(rpath, sizeof rpath, "tests/fixtures/deflate_%s.raw", names[i]);
                snprintf(zpath, sizeof zpath, "tests/fixtures/deflate_%s_dyn.zlib", names[i]);
                test_pair(zpath, rpath, false);
                snprintf(zpath, sizeof zpath, "tests/fixtures/deflate_%s_fix.zlib", names[i]);
                test_pair(zpath, rpath, false);
                snprintf(zpath, sizeof zpath, "tests/fixtures/deflate_%s_sto.zlib", names[i]);
                test_pair(zpath, rpath, false);
                snprintf(zpath, sizeof zpath, "tests/fixtures/deflate_%s_raw.deflate", names[i]);
                test_pair(zpath, rpath, true);
        }

        /* error paths */
        {
                size_t len = 0;
                u8 *z = slurp("tests/fixtures/deflate_trunc.zlib", &len);
                u8 *out = NULL;
                size_t outlen = 0;
                int rc = rev_inflate_zlib(z, len, 0, &out, &outlen);
                CHECK(rc != REV_INF_OK, "truncated stream should fail (rc=%d)", rc);
                CHECK(out == NULL, "truncated stream must not produce output");
                free(z);

                z = slurp("tests/fixtures/deflate_badcrc.zlib", &len);
                rc = rev_inflate_zlib(z, len, 0, &out, &outlen);
                CHECK(rc != REV_INF_OK, "bad adler stream should fail (rc=%d)", rc);
                free(z);
        }

        /* known-size fast path must not over/under read */
        {
                size_t len = 0;
                u8 *z = slurp("tests/fixtures/deflate_matches_dyn.zlib", &len);
                u8 *out = NULL;
                size_t outlen = 0;
                int rc = rev_inflate_zlib(z, len, 160000, &out, &outlen);
                CHECK(rc == REV_INF_OK && outlen == 160000, "known-size inflate failed");
                free(out);
                free(z);
        }

        /* the exact-size contract is a HARD CAP: `expected` smaller than the
         * real output must fail with REV_INF_TOOBIG (the decompression-bomb
         * stop) — never grow past the caller's budget, never half-succeed */
        {
                size_t len = 0;
                u8 *z = slurp("tests/fixtures/deflate_matches_dyn.zlib", &len);
                u8 *out = NULL;
                size_t outlen = 0;
                int rc = rev_inflate_zlib(z, len, 159999, &out, &outlen);
                CHECK(rc == REV_INF_TOOBIG,
                      "expected < actual must hit the cap (rc=%d: %s)", rc,
                      rev_inflate_strerror(rc));
                CHECK(out == NULL, "capped inflate must not produce output");
                free(z);
        }

        if (g_fails == 0)
                printf("  inflate: OK\n");
        return g_fails != 0;
}
