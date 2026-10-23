/* ============================================================================
 * Reverie Engine — tests/test_x11auth.c
 * Unit tests for the .Xauthority parser (x11_xauth_lookup): the real
 * record layout (BIG-ENDIAN u16 lengths, each directly preceding its
 * payload — interleaved, not a flat header), family/address/display
 * matching, and malformed-file safety.
 *
 * Fixtures are generated at runtime in the exact wire format a live
 * enforcing Xorg accepts (the e2e x11(auth) legs prove that half against
 * a real -auth server). The 2026-10 "server refused connection: Invalid"
 * field report shipped because the old parser read little-endian lengths
 * from a flat 12-byte header and rejected FamilyWild — every test below
 * is red on that parser by construction.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/x11.h"

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

static void put_be16(FILE *f, unsigned v)
{
        fputc((int)((v >> 8) & 0xff), f);
        fputc((int)(v & 0xff), f);
}

static void put_rec(FILE *f, unsigned family, const void *addr, unsigned alen,
                    const char *number, const char *name,
                    const void *data, unsigned dtlen)
{
        put_be16(f, family);
        put_be16(f, alen);
        if (alen)
                fwrite(addr, 1, alen, f);
        put_be16(f, (unsigned)strlen(number));
        fwrite(number, 1, strlen(number), f);
        put_be16(f, (unsigned)strlen(name));
        fwrite(name, 1, strlen(name), f);
        put_be16(f, dtlen);
        if (dtlen)
                fwrite(data, 1, dtlen, f);
}

static const u8 COOKIE_A[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};
static const u8 COOKIE_B[16] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
};
static const u8 COOKIE_C[32] = {
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
        0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f
};

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);

        char tmp[128];
        snprintf(tmp, sizeof tmp, "/tmp/revxauth.%d.bin", (int)getpid());
        u8 out[64];
        u32 outlen;

        /* ---- argument guards --------------------------------------------- */
        outlen = 99;
        CHECK(!x11_xauth_lookup(NULL, "", 0, out, &outlen) && outlen == 99,
              "NULL path rejected, out untouched");
        CHECK(!x11_xauth_lookup("", "", 0, out, &outlen), "empty path rejected");
        CHECK(!x11_xauth_lookup("/nonexistent/rev-xauth", "", 0, out, &outlen),
              "missing file rejected");
        CHECK(!x11_xauth_lookup(tmp, "", -1, out, &outlen),
              "negative display rejected");
        CHECK(!x11_xauth_lookup(tmp, NULL, 0, out, &outlen),
              "NULL host tolerated (treated local)");

        /* ---- FamilyWild: the `xauth add :N .` / startx / ssh shape -------- */
        /* This record alone proves big-endian reading: an LE parser reads the
         * name length (18 -> bytes 00 12) as 0x1200 = 4608 and bails. */
        {
                FILE *f = fopen(tmp, "wb");
                put_rec(f, 65535, "", 0, "96", "MIT-MAGIC-COOKIE-1",
                        COOKIE_A, 16);
                fclose(f);
        }
        outlen = 0;
        memset(out, 0xAA, sizeof out);
        CHECK(x11_xauth_lookup(tmp, "", 96, out, &outlen) &&
              outlen == 16 && memcmp(out, COOKIE_A, 16) == 0,
              "FamilyWild record found, cookie bytes exact (BE lengths)");

        CHECK(!x11_xauth_lookup(tmp, "", 97, out, &outlen),
              "display number mismatch rejected");
        CHECK(x11_xauth_lookup(tmp, "gpu-box", 96, out, &outlen),
              "FamilyWild is transport-agnostic (remote host still matches)");

        /* empty number field = any display */
        {
                FILE *f = fopen(tmp, "wb");
                put_rec(f, 65535, "", 0, "", "MIT-MAGIC-COOKIE-1", COOKIE_B, 16);
                fclose(f);
        }
        CHECK(x11_xauth_lookup(tmp, "", 0, out, &outlen) &&
              outlen == 16 && memcmp(out, COOKIE_B, 16) == 0,
              "empty number field matches any display");
        CHECK(x11_xauth_lookup(tmp, "", 42, out, &outlen),
              "empty number field matches display 42 too");

        /* ---- FamilyLocal: hostname keyed --------------------------------- */
        char host[256] = { 0 };
        gethostname(host, sizeof host - 1);
        {
                FILE *f = fopen(tmp, "wb");
                put_rec(f, 256, host, (unsigned)strlen(host), "0",
                        "MIT-MAGIC-COOKIE-1", COOKIE_A, 16);
                fclose(f);
        }
        CHECK(x11_xauth_lookup(tmp, "", 0, out, &outlen) &&
              outlen == 16 && memcmp(out, COOKIE_A, 16) == 0,
              "FamilyLocal with our hostname found");
        CHECK(x11_xauth_lookup(tmp, "unix", 0, out, &outlen),
              "FamilyLocal reachable via DISPLAY=unix:0");
        {
                FILE *f = fopen(tmp, "wb");
                put_rec(f, 256, "some-other-box", 14, "0",
                        "MIT-MAGIC-COOKIE-1", COOKIE_A, 16);
                fclose(f);
        }
        CHECK(!x11_xauth_lookup(tmp, "", 0, out, &outlen),
              "FamilyLocal with foreign hostname rejected");
        CHECK(!x11_xauth_lookup(tmp, "gpu-box", 0, out, &outlen),
              "FamilyLocal never offered to a remote DISPLAY");

        /* ---- FamilyInternet / FamilyInet6: loopback and foreign addrs ---- */
        {
                FILE *f = fopen(tmp, "wb");
                u8 lo[4] = { 127, 0, 0, 1 };
                put_rec(f, 0, lo, 4, "0", "MIT-MAGIC-COOKIE-1", COOKIE_A, 16);
                fclose(f);
        }
        CHECK(x11_xauth_lookup(tmp, "", 0, out, &outlen) &&
              memcmp(out, COOKIE_A, 16) == 0,
              "FamilyInternet 127.0.0.1 found for a local display");
        CHECK(x11_xauth_lookup(tmp, "localhost", 0, out, &outlen),
              "FamilyInternet loopback reachable via DISPLAY=localhost:0");
        {
                FILE *f = fopen(tmp, "wb");
                u8 foreign[4] = { 203, 0, 113, 7 };   /* TEST-NET-3 */
                put_rec(f, 0, foreign, 4, "0", "MIT-MAGIC-COOKIE-1", COOKIE_A, 16);
                fclose(f);
        }
        CHECK(!x11_xauth_lookup(tmp, "", 0, out, &outlen),
              "FamilyInternet foreign address rejected");
        {
                FILE *f = fopen(tmp, "wb");
                u8 lo6[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
                put_rec(f, 6, lo6, 16, "0", "MIT-MAGIC-COOKIE-1", COOKIE_A, 16);
                fclose(f);
        }
        CHECK(x11_xauth_lookup(tmp, "", 0, out, &outlen),
              "FamilyInternet6 ::1 found for a local display");
        {
                FILE *f = fopen(tmp, "wb");
                u8 doc[16] = { 0x20, 0x01, 0x0d, 0xb8, 0,0,0,0, 0,0,0,0, 0,0,0,7 };
                put_rec(f, 6, doc, 16, "0", "MIT-MAGIC-COOKIE-1", COOKIE_A, 16);
                fclose(f);
        }
        CHECK(!x11_xauth_lookup(tmp, "", 0, out, &outlen),
              "FamilyInternet6 documentation address rejected");

        /* ---- name and data sanity ----------------------------------------- */
        {
                FILE *f = fopen(tmp, "wb");
                put_rec(f, 65535, "", 0, "0", "XDM-AUTHORIZATION-1",
                        COOKIE_A, 16);
                fclose(f);
        }
        CHECK(!x11_xauth_lookup(tmp, "", 0, out, &outlen),
              "non-cookie auth schemes skipped");
        {
                FILE *f = fopen(tmp, "wb");
                u8 big[100];
                memset(big, 0x5A, sizeof big);
                put_rec(f, 65535, "", 0, "0", "MIT-MAGIC-COOKIE-1", big, 100);
                fclose(f);
        }
        CHECK(!x11_xauth_lookup(tmp, "", 0, out, &outlen),
              "oversized (100-byte) data rejected as a cookie");
        {
                FILE *f = fopen(tmp, "wb");
                put_rec(f, 65535, "", 0, "0", "MIT-MAGIC-COOKIE-1", COOKIE_C, 32);
                fclose(f);
        }
        CHECK(x11_xauth_lookup(tmp, "", 0, out, &outlen) && outlen == 32,
              "32-byte cookie accepted");

        /* ---- multi-record files: order, first match wins ------------------ */
        {
                FILE *f = fopen(tmp, "wb");
                put_rec(f, 65535, "", 0, "1", "MIT-MAGIC-COOKIE-1", COOKIE_A, 16);
                put_rec(f, 65535, "", 0, "7", "MIT-MAGIC-COOKIE-1", COOKIE_B, 16);
                fclose(f);
        }
        CHECK(x11_xauth_lookup(tmp, "", 7, out, &outlen) &&
              memcmp(out, COOKIE_B, 16) == 0,
              "non-matching records skipped, later match found");
        CHECK(!x11_xauth_lookup(tmp, "", 5, out, &outlen),
              "no matching display in a multi-record file");
        {
                FILE *f = fopen(tmp, "wb");
                put_rec(f, 65535, "", 0, "3", "MIT-MAGIC-COOKIE-1", COOKIE_A, 16);
                put_rec(f, 65535, "", 0, "3", "MIT-MAGIC-COOKIE-1", COOKIE_B, 16);
                fclose(f);
        }
        CHECK(x11_xauth_lookup(tmp, "", 3, out, &outlen) &&
              memcmp(out, COOKIE_A, 16) == 0,
              "first matching record wins (libXau file-order semantics)");

        /* ---- malformed files ----------------------------------------------- */
        {
                FILE *f = fopen(tmp, "wb");
                fclose(f);
        }
        CHECK(!x11_xauth_lookup(tmp, "", 0, out, &outlen),
              "empty file rejected");
        {
                FILE *f = fopen(tmp, "wb");
                put_be16(f, 65535);      /* family, then EOF mid-record */
                fclose(f);
        }
        CHECK(!x11_xauth_lookup(tmp, "", 0, out, &outlen),
              "truncated record rejected without crash");
        {
                FILE *f = fopen(tmp, "wb");
                put_rec(f, 65535, "", 0, "0", "MIT-MAGIC-COOKIE-1", COOKIE_A, 16);
                put_be16(f, 65535);      /* valid record + truncated tail */
                put_be16(f, 0);
                fclose(f);
        }
        CHECK(x11_xauth_lookup(tmp, "", 0, out, &outlen),
              "valid record before a truncated tail still found");

        unlink(tmp);

        if (failures == 0) {
                printf("x11auth: OK\n");
                return 0;
        }
        printf("x11auth: %d failure(s)\n", failures);
        return 1;
}
