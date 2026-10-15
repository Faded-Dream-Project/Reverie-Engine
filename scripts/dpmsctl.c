/* ============================================================================
 * Reverie Engine — scripts/dpmsctl.c
 * Minimal raw-wire X11 client for the DPMS extension: query and force the
 * display power level WITHOUT Xlib/XCB (same wire style as src/x11.c).
 *
 *   cc -O2 -o dpmsctl scripts/dpmsctl.c && ./dpmsctl info
 *   ./dpmsctl force off          (on | standby | suspend | off)
 *
 * Used by scripts/e2e.sh to drive real DPMS transitions against Xvfb (the
 * environment's xset is absent; this stands in for it). Exit codes:
 *   0 = command succeeded, 2 = server has no DPMS extension, 1 = other error.
 * Connects to $DISPLAY's unix socket with no auth (bare `Xvfb :N` accepts
 * that; xvfb-run servers with -auth need their cookie — not our test path).
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
#include <unistd.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

static int fd = -1;

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u16  get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static int write_all(const u8 *p, size_t n)
{
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

static int read_all(u8 *p, size_t n)
{
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

/* returns 0 on success (setup consumed), -1 on failure */
static int connect_and_setup(void)
{
        const char *dpy = getenv("DISPLAY");
        if (!dpy || !*dpy) {
                fprintf(stderr, "dpmsctl: DISPLAY is not set\n");
                return -1;
        }
        /* ":N[.S]" -> /tmp/.X11-unix/XN */
        const char *num = strchr(dpy, ':');
        if (!num)
                num = dpy;
        else
                num++;
        char path[64];
        snprintf(path, sizeof path, "/tmp/.X11-unix/X%s", num);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
                return -1;
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
                fprintf(stderr, "dpmsctl: cannot connect to %s (%s)\n",
                        path, strerror(errno));
                return -1;
        }

        /* connection setup, little-endian, no auth */
        u8 setup[12];
        memset(setup, 0, sizeof setup);
        setup[0] = 'l';
        put16(setup + 2, 11);      /* protocol-major */
        put16(setup + 4, 0);       /* protocol-minor */
        if (write_all(setup, sizeof setup) < 0)
                return -1;
        u8 hdr[8];
        if (read_all(hdr, sizeof hdr) < 0)
                return -1;
        if (hdr[0] != 1) {
                fprintf(stderr, "dpmsctl: server refused the connection "
                                "(setup status %u — auth?)\n", hdr[0]);
                return -1;
        }
        u16 extra = get16(hdr + 6);
        u8 sink[256 * 4];
        while (extra > 0) {
                u16 chunk = extra > 256 ? 256 : extra;
                if (read_all(sink, (size_t)chunk * 4) < 0)
                        return -1;
                extra -= chunk;
        }
        return 0;
}

/* lockstep reply reader: skips events */
static int read_reply(u8 *out32)
{
        for (;;) {
                u8 h[32];
                if (read_all(h, sizeof h) < 0)
                        return -1;
                if (h[0] == 0) {
                        fprintf(stderr, "dpmsctl: X error reply "
                                        "(code %u, major %u, minor %u)\n",
                                h[1], h[10], get16(h + 8));
                        return -1;
                }
                if (h[0] == 1) {
                        memcpy(out32, h, 32);
                        return 0;
                }
                /* event: drain */
        }
}

static int query_extension(const char *name, u8 *major_out)
{
        u8 q[32];
        memset(q, 0, sizeof q);
        u16 nlen = (u16)strlen(name);
        q[0] = 98;                     /* QueryExtension */
        put16(q + 2, (u16)(2 + (nlen + 3) / 4));
        put16(q + 4, nlen);
        memcpy(q + 8, name, nlen);
        if (write_all(q, (size_t)8 + nlen) < 0)
                return -1;
        u8 rep[32];
        if (read_reply(rep) < 0)
                return -1;
        if (!rep[8])
                return 0;              /* absent */
        *major_out = rep[9];
        return 1;
}

static int dpms_version(u8 major)
{
        u8 q[8];
        memset(q, 0, sizeof q);
        q[0] = major;
        q[1] = 0;                      /* GetVersion */
        put16(q + 2, 2);
        put16(q + 4, 2);
        put16(q + 6, 0);
        if (write_all(q, sizeof q) < 0)
                return -1;
        u8 rep[32];
        if (read_reply(rep) < 0)
                return -1;
        printf("dpms: server version %u.%u\n", get16(rep + 8), get16(rep + 10));
        return 0;
}

/* requests without replies: Enable(4), Disable(5), ForceLevel(6) */
static int dpms_noreply(u8 major, u8 minor, u16 level)
{
        u8 q[8];
        memset(q, 0, sizeof q);
        q[0] = major;
        q[1] = minor;
        put16(q + 2, 2);
        put16(q + 4, level);
        return write_all(q, sizeof q) < 0 ? -1 : 0;
}

/* GetInfo(7): state BOOL at byte 1, power_level CARD16 at offset 8 */
static int dpms_info(u8 major)
{
        u8 q[8];
        memset(q, 0, sizeof q);
        q[0] = major;
        q[1] = 7;
        put16(q + 2, 2);
        if (write_all(q, sizeof q) < 0)
                return -1;
        u8 rep[32];
        if (read_reply(rep) < 0)
                return -1;
        static const char *names[4] = { "on", "standby", "suspend", "off" };
        u16 level = get16(rep + 8);
        printf("dpms: state=%s level=%u (%s)\n", rep[1] ? "enabled" : "disabled",
               level, level < 4 ? names[level] : "unknown");
        return 0;
}

int main(int argc, char **argv)
{
        if (argc < 2) {
                fprintf(stderr, "usage: dpmsctl info | force on|standby|suspend|off\n");
                return 1;
        }
        if (connect_and_setup() < 0)
                return 1;

        u8 dpms = 0;
        int have = query_extension("DPMS", &dpms);
        if (have < 0)
                return 1;
        if (!have) {
                fprintf(stderr, "dpmsctl: server has no DPMS extension\n");
                return 2;
        }
        if (dpms_version(dpms) < 0)
                return 1;

        if (strcmp(argv[1], "info") == 0) {
                if (dpms_info(dpms) < 0)
                        return 1;
                return 0;
        }
        if (strcmp(argv[1], "force") == 0 && argc == 3) {
                u16 level = 0xFFFF;
                if (strcmp(argv[2], "on") == 0)             level = 0;
                else if (strcmp(argv[2], "standby") == 0)   level = 1;
                else if (strcmp(argv[2], "suspend") == 0)   level = 2;
                else if (strcmp(argv[2], "off") == 0)       level = 3;
                if (level == 0xFFFF) {
                        fprintf(stderr, "dpmsctl: bad level '%s'\n", argv[2]);
                        return 1;
                }
                /* ForceLevel is a no-op server-side while DPMS is disabled */
                if (dpms_noreply(dpms, 4, 0) < 0)           /* Enable */
                        return 1;
                if (dpms_noreply(dpms, 6, level) < 0)       /* ForceLevel */
                        return 1;
                if (dpms_info(dpms) < 0)
                        return 1;
                return 0;
        }
        fprintf(stderr, "usage: dpmsctl info | force on|standby|suspend|off\n");
        return 1;
}
