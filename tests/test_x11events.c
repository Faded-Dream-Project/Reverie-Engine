/* ============================================================================
 * Reverie Engine — tests/test_x11events.c
 * Pins the wire field offsets of the geometry-carrying X11 events the event
 * pump reads (Expose 12, ConfigureNotify 22) against synthetic protocol
 * buffers. The 2026-10 ConfigureNotify reader matched ABOVE-SIBLING as the
 * window and BORDER-WIDTH/OVERRIDE-REDIRECT as the size — resize tracking
 * could never fire on a real server, and no test had ever driven the bytes.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <stdio.h>
#include <string.h>

#include "x11.h"

static int g_fails = 0;

#define CHECK(cond, ...)                                                    \
        do {                                                                    \
                if (!(cond)) {                                                      \
                        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
                        printf(__VA_ARGS__);                                            \
                        printf("\n");                                                   \
                        g_fails++;                                                     \
                }                                                                   \
        } while (0)

static void put16(unsigned char *p, unsigned v)
{
        p[0] = (unsigned char)(v & 0xff);
        p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void put32(unsigned char *p, unsigned long v)
{
        put16(p, (unsigned)(v & 0xffff));
        put16(p + 2, (unsigned)((v >> 16) & 0xffff));
}

int main(void)
{
        printf("test_x11events:\n");

        /* Expose (12): window@4, x@8, y@10, width@12, height@14, count@16 */
        {
                unsigned char ev[32];
                memset(ev, 0, sizeof ev);
                ev[0] = 12;
                put32(ev + 4, 0x00a10020);       /* the exposed window      */
                put16(ev + 8, 320);              /* x                       */
                put16(ev + 10, 240);             /* y                       */
                put16(ev + 12, 640);             /* width                   */
                put16(ev + 14, 400);             /* height                  */
                put16(ev + 16, 3);               /* more exposes following  */
                put32(ev + 20, 0xdeadbeef);      /* unused — must be ignored */

                u32 win = 0;
                i32 x = -1, y = -1;
                u32 w = 0, h = 0;
                x11_event_geometry(ev, &win, &x, &y, &w, &h);
                CHECK(win == 0x00a10020, "expose window (got 0x%x)", win);
                CHECK(x == 320, "expose x (got %d)", x);
                CHECK(y == 240, "expose y (got %d)", y);
                CHECK(w == 640, "expose width (got %u)", w);
                CHECK(h == 400, "expose height (got %u)", h);
        }

        /* ConfigureNotify (22): event@4, window@8, above-sibling@12, x@16,
         * y@18, width@20, height@22 — the OLD reader took window@12
         * (above-sibling!) and size@24/26 (border-width/override). */
        {
                unsigned char ev[32];
                memset(ev, 0, sizeof ev);
                ev[0] = 22;
                put32(ev + 4, 0x00a10020);       /* event (we selected)     */
                put32(ev + 8, 0x00a10020);       /* window (the configured) */
                put32(ev + 12, 0x0000ffff);      /* above-sibling XID       */
                put16(ev + 16, 1920);
                put16(ev + 18, 1080);
                put16(ev + 20, 1280);            /* width                   */
                put16(ev + 22, 800);             /* height                  */
                put16(ev + 24, 0);               /* border-width            */
                ev[26] = 0;                      /* override-redirect       */

                u32 win = 0;
                i32 x = -1, y = -1;
                u32 w = 0, h = 0;
                x11_event_geometry(ev, &win, &x, &y, &w, &h);
                CHECK(win == 0x00a10020, "configure window (got 0x%x)", win);
                CHECK(x == 1920, "configure x (got %d)", x);
                CHECK(y == 1080, "configure y (got %d)", y);
                CHECK(w == 1280, "configure width — border-width is NOT the size (got %u)", w);
                CHECK(h == 800, "configure height — override byte is NOT the size (got %u)", h);
        }

        /* the false-match the old offsets produced: above-sibling naming an
         * unrelated window must not be readable as the configured window */
        {
                unsigned char ev[32];
                memset(ev, 0, sizeof ev);
                ev[0] = 22;
                put32(ev + 8, 0x11111111);       /* actual window           */
                put32(ev + 12, 0x22222222);      /* above-sibling           */
                put16(ev + 20, 500);
                put16(ev + 22, 400);
                u32 win = 0;
                x11_event_geometry(ev, &win, NULL, NULL, NULL, NULL);
                CHECK(win == 0x11111111, "window field, not above-sibling (got 0x%x)", win);
        }

        /* negative coordinates stay signed (windows left of / above root) */
        {
                unsigned char ev[32];
                memset(ev, 0, sizeof ev);
                ev[0] = 12;
                put32(ev + 4, 7);
                put16(ev + 8, 0xff9c);           /* -100 */
                put16(ev + 10, 0x100);           /* 256  */
                put16(ev + 12, 8);
                put16(ev + 14, 9);
                i32 x = 0, y = 0;
                x11_event_geometry(ev, NULL, &x, &y, NULL, NULL);
                CHECK(x == -100, "signed expose x (got %d)", x);
                CHECK(y == 256, "expose y (got %d)", y);
        }

        /* unknown codes zero every output */
        {
                unsigned char ev[32];
                memset(ev, 0, sizeof ev);
                ev[0] = 16;                      /* CreateNotify: not ours */
                put32(ev + 4, 0x0bad0bad);
                u32 win = 99;
                i32 x = 99, y = 99;
                u32 w = 99, h = 99;
                x11_event_geometry(ev, &win, &x, &y, &w, &h);
                CHECK(win == 0 && x == 0 && y == 0 && w == 0 && h == 0,
                      "unknown event zeroes outputs (got win=0x%x x=%d y=%d w=%u h=%u)",
                      win, x, y, w, h);
        }

        if (g_fails == 0)
                printf("  x11events: OK\n");
        return g_fails != 0;
}
