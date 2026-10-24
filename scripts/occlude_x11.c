/* ============================================================================
 * Reverie Engine — scripts/occlude_x11.c
 * A dependency-free raw X11 wire client for the e2e expose leg: it maps a
 * window over the left half of the root, holds it, unmaps it (the server
 * then repaints the wallpaper's exposed regions with its background and
 * sends Expose), waits, and dumps the ROOT pixels via GetImage so the
 * script can see what a user's eyes would see — reverie's own --shot
 * reads the wallpaper window's private content and CANNOT observe the gap.
 *
 * Usage: occlude_x11 WAIT_MS HOLD_MS SAMPLE_MS OUT.raw
 *   WAIT_MS   sleep before mapping (alignment with the daemon's pace)
 *   HOLD_MS   how long the occluder stays mapped
 *   SAMPLE_MS sleep after unmapping before the root screenshot
 * Prints "ROOT W H" on stdout; the raw file is W*H*4 bytes (depth-24
 * ZPixmap, 4 bytes/px, server byte order — blackness checks don't care).
 *
 * No Xlib, no XCB: compiled with plain cc inside scripts/e2e.sh. The e2e
 * Xvfb runs without -auth, so the setup handshake carries no credentials.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v)
{
        p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 get32(const u8 *p)
{
        return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int wr_all(int fd, const void *buf, size_t n)
{
        const u8 *p = (const u8 *)buf;
        while (n > 0) {
                ssize_t k = write(fd, p, n);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        return -1;
                }
                p += k;
                n -= (size_t)k;
        }
        return 0;
}

static int rd_all(int fd, void *buf, size_t n)
{
        u8 *p = (u8 *)buf;
        while (n > 0) {
                ssize_t k = read(fd, p, n);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        return -1;
                }
                if (k == 0)
                        return -1;
                p += k;
                n -= (size_t)k;
        }
        return 0;
}

static void msleep(long ms)
{
        struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
        if (argc != 5) {
                fprintf(stderr, "usage: %s WAIT_MS HOLD_MS SAMPLE_MS OUT.raw\n", argv[0]);
                return 2;
        }
        long wait_ms = atol(argv[1]);
        long hold_ms = atol(argv[2]);
        long sample_ms = atol(argv[3]);
        const char *out_path = argv[4];

        const char *display = getenv("DISPLAY");
        if (!display || !*display) {
                fprintf(stderr, "occlude: DISPLAY is not set\n");
                return 2;
        }
        const char *colon = strrchr(display, ':');
        if (!colon) {
                fprintf(stderr, "occlude: bad DISPLAY '%s'\n", display);
                return 2;
        }
        int dispnum = atoi(colon + 1);

        char path[64];
        snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", dispnum);
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
                fprintf(stderr, "occlude: cannot connect to %s\n", path);
                return 2;
        }

        /* setup request: little-endian, protocol 11.0, no auth (the e2e
         * Xvfb runs without -auth; a demanding server refuses us here) */
        u8 setup[12];
        memset(setup, 0, sizeof setup);
        setup[0] = 'l';
        put16(setup + 2, 11);
        if (wr_all(fd, setup, 12) != 0) {
                fprintf(stderr, "occlude: setup write failed\n");
                return 2;
        }
        u8 hdr[8];
        if (rd_all(fd, hdr, 8) != 0 || hdr[0] != 1) {
                fprintf(stderr, "occlude: server refused the connection\n");
                return 2;
        }
        u32 extra = get16(hdr + 6);
        u8 *body = (u8 *)malloc(extra ? (size_t)extra * 4 : 1);
        if (!body || (extra && rd_all(fd, body, (size_t)extra * 4) != 0)) {
                fprintf(stderr, "occlude: truncated setup reply\n");
                return 2;
        }
        /* release(4) rid_base(4) rid_mask(4) motion(4) vendor_len(2) ... */
        u32 rid_base = get32(body + 4);
        u32 rid_mask = get32(body + 8);
        u16 vendor_len = get16(body + 16);
        u8 nscreens = body[20];
        u8 nformats = body[21];
        u8 *p = body + 32 + vendor_len + ((4 - (vendor_len & 3)) & 3);
        p += (size_t)nformats * 8;
        if (nscreens < 1 || (rid_mask & 3) != 3) {
                fprintf(stderr, "occlude: no usable screen\n");
                return 2;
        }
        u32 root = get32(p);
        u32 root_w = get16(p + 20), root_h = get16(p + 22);

        u32 wid = rid_base;    /* first XID of OUR range — a raw constant
                                * draws BadIDChoice (XIDs are per-client) */

        /* CreateWindow: override-redirect, black, left half of the root */
        u8 q[48];
        memset(q, 0, sizeof q);
        q[0] = 1;                              /* CreateWindow */
        q[1] = 0;                              /* depth = CopyFromParent */
        put16(q + 2, 10);                      /* 40 bytes = 10 words      */
        put32(q + 4, wid);
        put32(q + 8, root);
        put16(q + 12, 0);                      /* x */
        put16(q + 14, 0);                      /* y */
        put16(q + 16, (u16)(root_w / 2));      /* width: left half  */
        put16(q + 18, (u16)root_h);            /* height            */
        put16(q + 20, 0);                      /* border width */
        q[22] = 1;                             /* class = InputOutput */
        put32(q + 24, 0);                      /* visual = CopyFromParent */
        put32(q + 28, (1u << 1) | (1u << 9));  /* CWBackPixel | CWOverrideRedirect */
        put32(q + 32, 0);                      /* background pixel: black */
        put32(q + 36, 1);                      /* override-redirect: true */
        if (wr_all(fd, q, 40) != 0)
                return 2;

        msleep(wait_ms);

        u8 m[8];
        memset(m, 0, sizeof m);
        m[0] = 8;                              /* MapWindow */
        put16(m + 2, 2);
        put32(m + 4, wid);
        if (wr_all(fd, m, 8) != 0)
                return 2;

        msleep(hold_ms);

        memset(m, 0, sizeof m);
        m[0] = 10;                             /* UnmapWindow */
        put16(m + 2, 2);
        put32(m + 4, wid);
        if (wr_all(fd, m, 8) != 0)
                return 2;

        /* give the daemon's Expose handling its window */
        msleep(sample_ms);

        /* GetImage(root, ZPixmap) */
        u8 gi[20];
        memset(gi, 0, sizeof gi);
        gi[0] = 73;                            /* GetImage */
        gi[1] = 2;                             /* ZPixmap */
        put16(gi + 2, 5);
        put32(gi + 4, root);
        put16(gi + 8, 0);
        put16(gi + 10, 0);
        put16(gi + 12, (u16)root_w);
        put16(gi + 14, (u16)root_h);
        put32(gi + 16, 0xFFFFFFFFu);           /* plane mask */
        if (wr_all(fd, gi, 20) != 0)
                return 2;

        u8 rh[32];
        if (rd_all(fd, rh, 32) != 0 || rh[0] != 1) {
                fprintf(stderr, "occlude: GetImage failed (code %u, err %u, seq %u)\n",
                        rh[0], rh[1], get16(rh + 2));
                return 2;
        }
        u32 len_words = get32(rh + 4);
        size_t data_bytes = (size_t)len_words * 4;
        u8 *data = (u8 *)malloc(data_bytes ? data_bytes : 1);
        if (!data || (data_bytes && rd_all(fd, data, data_bytes) != 0)) {
                fprintf(stderr, "occlude: truncated image data\n");
                return 2;
        }

        FILE *out = fopen(out_path, "wb");
        if (!out) {
                fprintf(stderr, "occlude: cannot write %s\n", out_path);
                return 2;
        }
        fwrite(data, 1, data_bytes, out);
        fclose(out);

        /* destroy the occluder */
        memset(m, 0, sizeof m);
        m[0] = 4;                              /* DestroyWindow */
        put16(m + 2, 2);
        put32(m + 4, wid);
        wr_all(fd, m, 8);

        printf("ROOT %u %u %zu\n", root_w, root_h, data_bytes);
        return 0;
}
