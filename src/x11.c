/* ============================================================================
 * Reverie Engine — x11.c
 * Raw X11 wire protocol. All multi-byte fields little-endian (we run on LE
 * hardware; byte-order is negotiated as 'l' during setup).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "x11.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

#define X11_MAX_WINS 8
#define REPLY_MAX    (64 * 1024)

typedef struct {
        u32 wid, gc;
        u32 w, h;
        bool use_shm;
        int cur_seg;
        struct {
                int shmid;
                u32 shmseg;
                u8 *mem;
                size_t size;
        } shm[2];
        u8 *fallback;
} XWin;

struct X11 {
        int fd;
        u32 rid_base, rid_mask, rid_next;
        u32 root;
        u32 depth, bpp;
        u32 rmask, gmask, bmask;
        int rshift, gshift, bshift;
        int rloss, gloss, bloss;
        u16 ext_shm, ext_xinerama;      /* major opcodes, 0 = absent */
        u16 ext_shm_ev;                 /* first event of MIT-SHM    */
        bool is_xwayland;               /* "XWAYLAND" extension present */
        XWin wins[X11_MAX_WINS];
        int nwins;
        /* event queue (events drained while waiting for replies) */
        u8 evq[X11_MAX_WINS * 16][32];
        int evq_n;
        u8 *reply;
        size_t reply_cap;
        /* latched button edges (consume-on-read via x11_pointer) */
        bool ppressed, press_edge, release_edge, click, click_pending;
        f64 click_x, click_y;
        f64 click_q[4][2];   /* latched clicks between reads (oldest first) */
        u32 click_qn;
        u32 clicks;
};

/* --------------------------------------------------------------------------
 * socket helpers
 * ------------------------------------------------------------------------ */

static bool write_all(int fd, const void *buf, size_t n)
{
        const u8 *p = (const u8 *)buf;
        while (n > 0) {
                ssize_t k = write(fd, p, n);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        return false;
                }
                p += k;
                n -= (size_t)k;
        }
        return true;
}

static bool read_all(int fd, void *buf, size_t n)
{
        u8 *p = (u8 *)buf;
        while (n > 0) {
                ssize_t k = read(fd, p, n);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        return false;
                }
                if (k == 0)
                        return false;
                p += k;
                n -= (size_t)k;
        }
        return true;
}

/* --------------------------------------------------------------------------
 * request/reply plumbing (strict lockstep)
 * ------------------------------------------------------------------------ */

static u16 get16(const u8 *p);
static u32 get32(const u8 *p);

static bool send_req(X11 *x, const void *buf, size_t n)
{
        /* pad to 4 */
        u8 pad[3] = { 0 };
        size_t rem = (4 - (n & 3)) & 3;
        if (!write_all(x->fd, buf, n))
                return false;
        if (rem && !write_all(x->fd, pad, rem))
                return false;
        return true;
}

/* Reads the next reply, buffering any events that arrive first.
 * `extra_hint`: if true, the reply length field is honored (variable data).
 * Returns pointer to the 32-byte reply header (x->reply). */
static u8 *read_reply(X11 *x, bool variable)
{
        for (;;) {
                u8 hdr[32];
                if (!read_all(x->fd, hdr, 32))
                        return NULL;
                u8 code = hdr[0];
                if (code == 0) {
                        /* error reply: this is the response to our pending request
                         * in the lockstep model — report failure to the caller. */
                        LOGD("x11: error reply (code %u, major %u, minor %u)",
                             hdr[1], hdr[10], get16(hdr + 8));
                        return NULL;
                }
                if (code == 1) {
                        memcpy(x->reply, hdr, 32);
                        if (variable) {
                                u32 len = get32(hdr + 4);
                                size_t need = (size_t)len * 4 + 32;
                                if (need > x->reply_cap) {
                                        u8 *nb = (u8 *)realloc(x->reply, need);
                                        if (!nb)
                                                return NULL;
                                        x->reply = nb;
                                        x->reply_cap = need;
                                        memcpy(x->reply, hdr, 32);
                                }
                                if (len && !read_all(x->fd, x->reply + 32, (size_t)len * 4))
                                        return NULL;
                        }
                        return x->reply;
                }
                /* event: buffer it */
                if (x->evq_n < (int)(sizeof x->evq / sizeof x->evq[0]))
                        memcpy(x->evq[x->evq_n++], hdr, 32);
        }
}

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static void put32(u8 *p, u32 v)
{
        p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static u32 get32(const u8 *p)
{
        return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 new_id(X11 *x)
{
        u32 id = x->rid_base + (x->rid_next & x->rid_mask);
        x->rid_next++;
        return id;
}

/* --------------------------------------------------------------------------
 * Xauthority
 * ------------------------------------------------------------------------ */

static bool xauth_lookup(int display, u8 *out, u32 *outlen)
{
        const char *path = getenv("XAUTHORITY");
        char homepath[512];
        if (!path || !*path) {
                const char *home = getenv("HOME");
                if (!home)
                        return false;
                snprintf(homepath, sizeof homepath, "%s/.Xauthority", home);
                path = homepath;
        }
        FILE *f = fopen(path, "rb");
        if (!f)
                return false;

        char host[256] = { 0 };
        gethostname(host, sizeof host - 1);

        bool found = false;
        for (;;) {
                u16 family, alen, dlen, nlen, dtlen;
                u8 hdr[12];
                if (fread(hdr, 1, 12, f) != 12)
                        break;
                family = (u16)(hdr[0] | (hdr[1] << 8));
                alen = (u16)(hdr[2] | (hdr[3] << 8));
                dlen = (u16)(hdr[4] | (hdr[5] << 8));
                nlen = (u16)(hdr[6] | (hdr[7] << 8));
                dtlen = (u16)(hdr[8] | (hdr[9] << 8));
                (void)dlen;
                if (alen > 4096 || nlen > 256 || dtlen > 256)
                        break;
                u8 addr[4096];
                if (alen && fread(addr, 1, alen, f) != alen)
                        break;
                u8 disp[64] = { 0 };
                if (dlen && fread(disp, 1, dlen, f) != (size_t)dlen)
                        break;
                u8 name[256];
                if (nlen && fread(name, 1, nlen, f) != (size_t)nlen)
                        break;
                u8 data[256];
                if (dtlen && fread(data, 1, dtlen, f) != (size_t)dtlen)
                        break;

                if (nlen == 18 && memcmp(name, "MIT-MAGIC-COOKIE-1", 18) == 0 &&
                    dtlen > 0 && dtlen <= 64) {
                        bool fam_ok = (family == 256) || (family == 1) || (family == 0);
                        bool disp_ok = true;
                        if (dlen > 0) {
                                char d[32];
                                snprintf(d, sizeof d, "%d", display);
                                disp_ok = (dlen == strlen(d) && memcmp(disp, d, dlen) == 0);
                        }
                        bool addr_ok = true;
                        if (family == 1 && alen > 0)
                                addr_ok = (alen == strlen(host) && memcmp(addr, host, alen) == 0);
                        if (fam_ok && disp_ok && addr_ok) {
                                memcpy(out, data, (size_t)dtlen);
                                *outlen = dtlen;
                                found = true;
                                break;
                        }
                }
        }
        fclose(f);
        return found;
}

/* --------------------------------------------------------------------------
 * connection setup
 * ------------------------------------------------------------------------ */

X11 *x11_connect(char *err, size_t errcap)
{
        const char *display = getenv("DISPLAY");
        if (!display || !*display) {
                snprintf(err, errcap, "x11: DISPLAY is not set");
                return NULL;
        }

        /* parse [host]:display[.screen] */
        const char *colon = strrchr(display, ':');
        if (!colon) {
                snprintf(err, errcap, "x11: bad DISPLAY '%s'", display);
                return NULL;
        }
        char host[128] = { 0 };
        size_t hlen = (size_t)(colon - display);
        if (hlen >= sizeof host)
                hlen = sizeof host - 1;
        memcpy(host, display, hlen);
        int dispnum = atoi(colon + 1);
        if (dispnum < 0)
                dispnum = 0;

        int fd = -1;
        char sockpath[108];   /* sockaddr_un limit */
        snprintf(sockpath, sizeof sockpath, "/tmp/.X11-unix/X%d", dispnum);
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        memcpy(sa.sun_path, sockpath, sizeof sa.sun_path);

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
                snprintf(err, errcap, "x11: socket(): %s", strerror(errno));
                return NULL;
        }
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
                close(fd);
                snprintf(err, errcap, "x11: cannot connect to %s (%s)", sockpath, strerror(errno));
                return NULL;
        }

        /* auth cookie */
        u8 cookie[64];
        u32 cookie_len = 0;
        bool have_cookie = xauth_lookup(dispnum, cookie, &cookie_len);

        /* setup request */
        u8 req[128];
        memset(req, 0, sizeof req);
        u16 namelen = 18, datalen = (u16)cookie_len;
        req[0] = 'l';                       /* little endian */
        put16(req + 2, 11);                 /* protocol major */
        put16(req + 4, 0);
        put16(req + 6, namelen);
        put16(req + 8, datalen);
        memcpy(req + 12, "MIT-MAGIC-COOKIE-1", 18);
        if (have_cookie)
                memcpy(req + 32, cookie, cookie_len);
        size_t reqlen = 32 + ((datalen + 3) & ~3u);
        if (!write_all(fd, req, reqlen)) {
                close(fd);
                snprintf(err, errcap, "x11: setup write failed");
                return NULL;
        }

        u8 hdr[8];
        if (!read_all(fd, hdr, 8)) {
                close(fd);
                snprintf(err, errcap, "x11: connection closed during setup");
                return NULL;
        }
        if (hdr[0] == 0) {
                /* failed: reason string follows */
                u16 len = (u16)(hdr[6] | (hdr[7] << 8));
                char reason[128] = { 0 };
                if (len > 0) {
                        size_t got = read(fd, reason, len < sizeof reason ? len : sizeof reason - 1);
                        (void)got;
                }
                close(fd);
                snprintf(err, errcap, "x11: server refused connection: %s", reason);
                return NULL;
        }
        if (hdr[0] == 2) {
                /* authenticate — not supported */
                close(fd);
                snprintf(err, errcap, "x11: server requires an unsupported auth mode");
                return NULL;
        }

        u32 extra = get16(hdr + 6);   /* u16 at bytes 6-7: 4-byte units after the 8-byte header */
        if (extra * 4 > REPLY_MAX - 8) {
                close(fd);
                snprintf(err, errcap, "x11: setup reply too large");
                return NULL;
        }
        u8 *body = (u8 *)malloc(extra ? (size_t)extra * 4 : 1);
        if (!body || (extra && !read_all(fd, body, extra * 4))) {
                free(body);
                close(fd);
                snprintf(err, errcap, "x11: truncated setup reply");
                return NULL;
        }

        X11 *x = (X11 *)calloc(1, sizeof(X11));
        x->fd = fd;
        x->reply = (u8 *)malloc(REPLY_MAX);
        x->reply_cap = REPLY_MAX;
        if (!x->reply) {
                free(x);
                close(fd);
                snprintf(err, errcap, "x11: out of memory");
                return NULL;
        }

        /* parse setup body
         * release(4) rid-base(4) rid-mask(4) motion(4) vendor-len(2) max-req(2)
         * n-screens(2) n-formats(2) byte-order(1) bit-order(1) scan-unit(1)
         * scan-pad(1) min-key(1) max-key(1) pad(2) = 32 bytes            */
        u32 rid_base = get32(body + 4);
        u32 rid_mask = get32(body + 8);
        u16 vendor_len = get16(body + 16);
        u16 max_req = get16(body + 18);
        /* X.Org wire format (verified against a live server): the screen and
         * format counts are single bytes at body[20] and body[21], followed by
         * byte orders, scanline pads, keycodes, then 4 unused bytes. */
        u8 nscreens = body[20];
        u8 nformats = body[21];
        (void)max_req;

        u8 *p = body + 32 + vendor_len + ((4 - (vendor_len & 3)) & 3);
        p += (size_t)nformats * 8;

        if (nscreens < 1) {
                snprintf(err, errcap, "x11: no screens");
                goto fail;
        }

        /* first screen: root(0) cmap(4) white(8) black(12) masks(16) w(20)
         * h(22) mmw(24) mmh(26) minmaps(28) maxmaps(30) visual(32) backing(36)
         * saveunder(37) root-depth(38) n-depths(39)                                  */
        x->root = get32(p);
        u32 root_w = get16(p + 20), root_h = get16(p + 22);
        u8 root_depth = p[38];
        u8 ndepths = p[39];
        (void)root_w; (void)root_h;

        /* find a TrueColor visual (prefer the root depth, then 24, then 32) */
        u8 *dp = p + 40;
        u32 best_visual = 0;
        int best_score = -1;
        u32 best_depth = 24;
        for (int d = 0; d < ndepths; d++) {
                u8 depth = dp[0];
                u16 nvis = get16(dp + 2);
                u8 *vp = dp + 8;
                for (int v = 0; v < nvis; v++) {
                        u32 vid = get32(vp);
                        u8 class_ = vp[4];
                        u32 rm = get32(vp + 8), gm = get32(vp + 12), bm = get32(vp + 16);
                        if (class_ == 4) {   /* TrueColor */
                                int score = (depth == root_depth) ? 4
                                            : (depth == 24) ? 3 : (depth == 32) ? 2 : 1;
                                if (score > best_score) {
                                        best_score = score;
                                        best_visual = vid;
                                        best_depth = depth;
                                        x->rmask = rm;
                                        x->gmask = gm;
                                        x->bmask = bm;
                                }
                        }
                        vp += 24;
                }
                dp += 8 + (size_t)nvis * 24;
        }
        if (best_score < 0) {
                snprintf(err, errcap, "x11: no TrueColor visual found");
                goto fail;
        }
        x->depth = best_depth;
        (void)best_visual;

        /* channel shifts/losses from masks */
        x->rshift = 0, x->gshift = 0, x->bshift = 0;
        x->rloss = 8; x->gloss = 8; x->bloss = 8;
        u32 masks[3] = { x->rmask, x->gmask, x->bmask };
        int *shifts[3] = { &x->rshift, &x->gshift, &x->bshift };
        int *losses[3] = { &x->rloss, &x->gloss, &x->bloss };
        for (int c = 0; c < 3; c++) {
                u32 m = masks[c];
                if (!m)
                        continue;
                int sh = 0;
                while (!(m & 1)) { m >>= 1; sh++; }
                int bits = 0;
                while (m & 1) { m >>= 1; bits++; }
                *shifts[c] = sh;
                *losses[c] = 8 - bits > 0 ? 8 - bits : 0;
        }
        /* assume 32bpp ZPixmap (true for every modern server at depth 24/32) */
        x->bpp = 32;

        x->rid_base = rid_base;
        x->rid_mask = rid_mask;
        x->rid_next = 1;

        /* extensions */
        const char *exts[] = { "MIT-SHM", "XINERAMA", "XWAYLAND" };
        u16 *bases[] = { &x->ext_shm, &x->ext_xinerama, NULL };
        u16 *firsts[] = { &x->ext_shm_ev, NULL, NULL };
        for (int e = 0; e < 3; e++) {
                u8 q[32];
                memset(q, 0, sizeof q);
                u16 nlen = (u16)strlen(exts[e]);
                q[0] = 98;   /* QueryExtension */
                put16(q + 2, (u16)(2 + (nlen + 3) / 4));
                put16(q + 4, nlen);
                memcpy(q + 8, exts[e], nlen);
                if (!send_req(x, q, 8 + nlen))
                        goto conn_fail;
                u8 *rep = read_reply(x, false);
                if (rep && rep[8]) {
                        if (bases[e])
                                *bases[e] = rep[9];
                        if (firsts[e])
                                *firsts[e] = rep[10];
                        if (e == 2) {
                                /* XWayland registers a fake "XWAYLAND"
                                 * extension — its presence means we are on a
                                 * Wayland session's X compatibility server.
                                 * Diagnostic only: the desktop-window
                                 * strategy already works there. */
                                x->is_xwayland = true;
                                LOGI("x11: XWayland detected (Wayland session's X server)");
                        } else {
                                LOGI("x11: extension %s = opcode %u", exts[e], rep[9]);
                        }
                }
        }

        /* Xinerama handshake (server requires QueryVersion before use) */
        if (x->ext_xinerama) {
                u8 q[8];
                memset(q, 0, sizeof q);
                q[0] = (u8)x->ext_xinerama;   /* major = extension base */
                q[1] = 0;                     /* minor = QueryVersion   */
                put16(q + 2, 2);
                put16(q + 4, 1);              /* client version 1.1 */
                put16(q + 6, 1);
                if (send_req(x, q, 8)) {
                        u8 *rep = read_reply(x, false);
                        if (rep)
                                LOGI("x11: Xinerama server version %u.%u",
                                     get16(rep + 8), get16(rep + 10));
                }
        }

        free(body);
        return x;

conn_fail:
        snprintf(err, errcap, "x11: connection lost during setup");
fail:
        free(body);
        free(x);
        close(fd);
        return NULL;
}

void x11_disconnect(X11 *x)
{
        if (!x)
                return;
        for (int i = 0; i < x->nwins; i++) {
                XWin *w = &x->wins[i];
                for (int s = 0; s < 2; s++) {
                        if (w->shm[s].shmid > 0) {
                                shmdt(w->shm[s].mem);
                                shmctl(w->shm[s].shmid, IPC_RMID, NULL);
                        }
                }
                free(w->fallback);
        }
        free(x->reply);    /* 2026-10 leak audit: the persistent lockstep
                           * reply buffer (grows to frame size for the
                           * GetImage screenshot path) was never freed */
        close(x->fd);
        free(x);
}

/* --------------------------------------------------------------------------
 * atoms / properties
 * ------------------------------------------------------------------------ */

static u32 intern_atom(X11 *x, const char *name)
{
        u8 q[64];
        size_t nlen = strlen(name);
        if (nlen > 40)
                return 0;
        memset(q, 0, sizeof q);
        q[0] = 16;   /* InternAtom */
        put16(q + 2, (u16)(2 + (nlen + 3) / 4));
        put16(q + 4, (u16)nlen);
        memcpy(q + 8, name, nlen);
        if (!send_req(x, q, 8 + nlen))
                return 0;
        u8 *rep = read_reply(x, false);
        if (!rep)
                return 0;
        return get32(rep + 8);
}

static bool change_property_atoms(X11 *x, u32 wid, u32 prop, const u32 *vals, u32 n)
{
        u8 q[64];
        memset(q, 0, sizeof q);
        q[0] = 18;   /* ChangeProperty */
        q[1] = 0;    /* Replace */
        put16(q + 2, (u16)(6 + n));   /* 24 + 4n bytes */
        put32(q + 4, wid);
        put32(q + 8, prop);
        put32(q + 12, 4);   /* type ATOM */
        q[16] = 32;         /* format */
        put32(q + 20, n);
        for (u32 i = 0; i < n && 24 + 4 * i + 4 <= sizeof q; i++)
                put32(q + 24 + 4 * i, vals[i]);
        return send_req(x, q, 24 + 4 * n);
}

/* --------------------------------------------------------------------------
 * monitors
 * ------------------------------------------------------------------------ */

void x11_root_size(X11 *x, u32 *w, u32 *h)
{
        /* re-query via GetGeometry on root */
        u8 q[16];
        memset(q, 0, sizeof q);
        q[0] = 14;   /* GetGeometry */
        put16(q + 2, 2);
        put32(q + 4, x->root);
        if (!send_req(x, q, 8))
                return;
        u8 *rep = read_reply(x, false);
        if (!rep)
                return;
        /* GetGeometry reply: depth(1)@1, seq(2), len(4), root(4)@8,
         * x(2)@12, y(2)@14, width(2)@16, height(2)@18, border(2)@20 */
        *w = get16(rep + 16);
        *h = get16(rep + 18);
}

int x11_monitors(X11 *x, RRect *out, int max)
{
        if (x->ext_xinerama) {
                u8 q[8];
                memset(q, 0, sizeof q);
                q[0] = (u8)x->ext_xinerama;   /* major = extension base */
                q[1] = 5;                     /* minor = QueryScreens    */
                put16(q + 2, 1);
                if (send_req(x, q, 4)) {
                        u8 *rep = read_reply(x, true);
                        if (rep) {
                                u32 n = get32(rep + 8);
                                if (n > (u32)max)
                                        n = (u32)max;
                                u8 *p = rep + 32;
                                for (u32 i = 0; i < n; i++) {
                                        out[i].x = get16(p);
                                        out[i].y = get16(p + 2);
                                        out[i].w = get16(p + 4);
                                        out[i].h = get16(p + 6);
                                        p += 8;
                                }
                                if (n > 0)
                                        return (int)n;
                        }
                }
        }
        /* fallback: whole root */
        u32 w = 1920, h = 1080;
        x11_root_size(x, &w, &h);
        if (max < 1)
                return 0;
        out[0].x = 0;
        out[0].y = 0;
        out[0].w = (i32)w;
        out[0].h = (i32)h;
        return 1;
}

/* --------------------------------------------------------------------------
 * windows
 * ------------------------------------------------------------------------ */

/* Creates a window (server resources + SHM) into slot `w`. Shared by
 * x11_add_window and x11_resize_window. */
static bool xwin_create(X11 *x, XWin *w, RRect geom)
{
        memset(w, 0, sizeof *w);
        w->w = (u32)geom.w;
        w->h = (u32)geom.h;

        u32 wid = new_id(x);
        u32 gc = new_id(x);

        /* CreateWindow */
        u8 q[64];
        memset(q, 0, sizeof q);
        q[0] = 1;                 /* CreateWindow */
        q[1] = 0;                 /* depth = CopyFromParent */
        put16(q + 2, 10);         /* 40 bytes total */
        put32(q + 4, wid);
        put32(q + 8, x->root);
        put16(q + 12, (u16)geom.x);
        put16(q + 14, (u16)geom.y);
        put16(q + 16, (u16)geom.w);
        put16(q + 18, (u16)geom.h);
        put16(q + 20, 0);         /* border width */
        q[22] = 1;                /* class = InputOutput */
        q[23] = 0;                /* pad */
        /* visual = CopyFromParent (0) at q[24], zeroed already */
        u32 mask = (1u << 1)      /* CWBackPixel */
                   | (1u << 11);  /* CWEventMask */
        put32(q + 24, 0);         /* visual: CopyFromParent */
        put32(q + 28, mask);
        put32(q + 32, 0);         /* background pixel: black */
        put32(q + 36, (1u << 15)  /* ExposureMask */
                      | (1u << 17)/* StructureNotifyMask */
                      | (1u << 22)/* PropertyChangeMask */
                      | (1u << 4) /* ButtonPressMask   */
                      | (1u << 5));/* ButtonReleaseMask */
        if (!send_req(x, q, 40))
                return false;

        /* CreateGC */
        memset(q, 0, sizeof q);
        q[0] = 55;                /* CreateGC */
        put16(q + 2, 4);
        put32(q + 4, gc);
        put32(q + 8, wid);
        put32(q + 12, 0);         /* no values */
        if (!send_req(x, q, 16))
                return false;

        /* EWMH: desktop window */
        u32 a_type = intern_atom(x, "_NET_WM_WINDOW_TYPE");
        u32 a_desktop = intern_atom(x, "_NET_WM_WINDOW_TYPE_DESKTOP");
        u32 a_state = intern_atom(x, "_NET_WM_STATE");
        u32 a_sticky = intern_atom(x, "_NET_WM_STATE_STICKY");
        u32 a_below = intern_atom(x, "_NET_WM_STATE_BELOW");
        u32 a_skipT = intern_atom(x, "_NET_WM_STATE_SKIP_TASKBAR");
        u32 a_skipP = intern_atom(x, "_NET_WM_STATE_SKIP_PAGER");
        if (a_type && a_desktop) {
                u32 v = a_desktop;
                change_property_atoms(x, wid, a_type, &v, 1);
        }
        if (a_state) {
                u32 v[4] = { a_sticky, a_below, a_skipT, a_skipP };
                u32 n = 0;
                for (u32 i = 0; i < 4; i++)
                        if (v[i])
                                v[n++] = v[i];
                if (n)
                        change_property_atoms(x, wid, a_state, v, n);
        }

        /* MapWindow */
        memset(q, 0, sizeof q);
        q[0] = 8;
        put16(q + 2, 2);
        put32(q + 4, wid);
        if (!send_req(x, q, 8))
                return false;

        /* MIT-SHM setup: two shared segments */
        w->use_shm = false;
        if (x->ext_shm) {
                size_t size = (size_t)w->w * w->h * 4;
                for (int s = 0; s < 2; s++) {
                        int shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
                        if (shmid < 0)
                                break;
                        void *mem = shmat(shmid, NULL, 0);
                        if (mem == (void *)-1) {
                                shmctl(shmid, IPC_RMID, NULL);
                                break;
                        }
                        shmctl(shmid, IPC_RMID, NULL);   /* auto-remove when detached */
                        u32 shmseg = new_id(x);
                        u8 a[16];
                        memset(a, 0, sizeof a);
                        a[0] = (u8)x->ext_shm;         /* major = MIT-SHM base */
                a[1] = 1;                      /* minor = ShmAttach     */
                        put16(a + 2, 4);               /* 16 bytes = 4 words */
                        put32(a + 4, shmseg);
                        put32(a + 8, (u32)shmid);
                        a[12] = 0;   /* read-only = false */
                        if (!send_req(x, a, 16))
                                break;
                        w->shm[s].shmid = shmid;
                        w->shm[s].shmseg = shmseg;
                        w->shm[s].mem = (u8 *)mem;
                        w->shm[s].size = size;
                        if (s == 1)
                                w->use_shm = true;
                }
        }
        if (!w->use_shm) {
                for (int s = 0; s < 2; s++) {
                        if (w->shm[s].shmid > 0) {
                                shmdt(w->shm[s].mem);
                                memset(&w->shm[s], 0, sizeof w->shm[s]);
                        }
                }
                w->fallback = (u8 *)malloc((size_t)w->w * w->h * 4);
                if (!w->fallback)
                        return false;
                LOGI("x11: MIT-SHM unavailable, using core PutImage");
        }

        w->wid = wid;
        w->gc = gc;
        x11_sync(x);
        return true;
}

int x11_add_window(X11 *x, RRect geom)
{
        if (x->nwins >= X11_MAX_WINS)
                return -1;
        XWin *w = &x->wins[x->nwins];
        if (!xwin_create(x, w, geom))
                return -1;
        return x->nwins++;
}

/* Destroy + re-create the window in slot `win` (new XID, same index). */
bool x11_resize_window(X11 *x, int win, RRect geom)
{
        if (!x || win < 0 || win >= x->nwins)
                return false;
        XWin *w = &x->wins[win];

        /* destroy the old server window */
        if (w->wid) {
                u8 q[8];
                memset(q, 0, sizeof q);
                q[0] = 4;            /* DestroyWindow */
                put16(q + 2, 2);
                put32(q + 4, w->wid);
                send_req(x, q, 8);
        }
        /* detach shm segments */
        for (int s = 0; s < 2; s++) {
                if (w->shm[s].shmseg) {
                        u8 a[12];
                        memset(a, 0, sizeof a);
                        a[0] = (u8)x->ext_shm;
                        a[1] = 2;    /* ShmDetach */
                        put16(a + 2, 3);
                        put32(a + 4, w->shm[s].shmseg);
                        send_req(x, a, 12);
                }
                if (w->shm[s].shmid > 0)
                        shmdt(w->shm[s].mem);
        }
        free(w->fallback);
        w->fallback = NULL;
        return xwin_create(x, w, geom);
}

/* --------------------------------------------------------------------------
 * presentation
 * ------------------------------------------------------------------------ */

static void convert_pixels(const X11 *x, const u32 *src, u8 *dst, size_t n)
{
        int rs = x->rshift, gs = x->gshift, bs = x->bshift;
        int rl = x->rloss, gl = x->gloss, bl = x->bloss;

        if (rs == 16 && gs == 8 && bs == 0 && rl == 0 && gl == 0 && bl == 0) {
                /* fast path: masks 0xFF0000 / 0xFF00 / 0xFF */
                for (size_t i = 0; i < n; i++) {
                        u32 p = src[i];
                        u32 v = ((p & 0xff) << 16) | (((p >> 8) & 0xff) << 8) | ((p >> 16) & 0xff);
                        ((u32 *)dst)[i] = v;
                }
                return;
        }
        for (size_t i = 0; i < n; i++) {
                u32 p = src[i];
                u32 r = (p & 0xff) >> rl;
                u32 g = ((p >> 8) & 0xff) >> gl;
                u32 b = ((p >> 16) & 0xff) >> bl;
                ((u32 *)dst)[i] = (r << rs) | (g << gs) | (b << bs);
        }
}

void x11_sync(X11 *x)
{
        u8 q[4];
        q[0] = 43;   /* GetInputFocus */
        put16(q + 2, 1);
        if (send_req(x, q, 4))
                read_reply(x, false);
}

bool x11_present(X11 *x, int win, const u32 *rgba, u32 w, u32 h)
{
        if (!x || win < 0 || win >= x->nwins)
                return false;
        XWin *wd = &x->wins[win];
        if (w != wd->w || h != wd->h || !rgba)
                return false;
        size_t n = (size_t)w * h;

        if (wd->use_shm) {
                int seg = wd->cur_seg;
                x11_sync(x);   /* ensure the server is not still reading this segment */
                convert_pixels(x, rgba, wd->shm[seg].mem, n);

                u8 q2[40];
                memset(q2, 0, sizeof q2);
                q2[0] = (u8)x->ext_shm;        /* major = MIT-SHM base */
                q2[1] = 3;                     /* minor = ShmPutImage   */
                put16(q2 + 2, 10);              /* 36 bytes = 9 words    */
                put32(q2 + 4, wd->wid);
                put32(q2 + 8, wd->gc);
                put16(q2 + 12, (u16)w);        /* total width  */
                put16(q2 + 14, (u16)h);        /* total height */
                put16(q2 + 16, 0);             /* src x */
                put16(q2 + 18, 0);             /* src y */
                put16(q2 + 20, (u16)w);        /* src width */
                put16(q2 + 22, (u16)h);        /* src height */
                q2[28] = (u8)x->depth;         /* layout per live capture: 4 pad bytes at 24..27 */
                q2[29] = 2;                    /* ZPixmap */
                q2[30] = 0;                    /* send event */
                q2[31] = 0;                    /* pad */
                put32(q2 + 32, wd->shm[seg].shmseg);
                put32(q2 + 36, 0);             /* offset */
                if (!send_req(x, q2, 40))
                        return false;
                wd->cur_seg ^= 1;
                return true;
        }

        /* core PutImage fallback, in <= 256-row chunks */
        convert_pixels(x, rgba, wd->fallback, n);
        u32 rows_per = 65536 / (w * 4);
        if (rows_per == 0) rows_per = 1;
        if (rows_per > h) rows_per = h;
        for (u32 y0 = 0; y0 < h; y0 += rows_per) {
                u32 rows = rows_per < h - y0 ? rows_per : h - y0;
                size_t nbytes = (size_t)w * rows * 4;
                u16 len = (u16)((24 + nbytes + 3) / 4);
                u8 *q = (u8 *)malloc(24);
                if (!q)
                        return false;
                memset(q, 0, 24);
                q[0] = 72;          /* PutImage */
                q[1] = 2;           /* ZPixmap */
                put16(q + 2, len);
                put32(q + 4, wd->wid);
                put32(q + 8, wd->gc);
                put16(q + 12, (u16)w);
                put16(q + 14, (u16)rows);
                put16(q + 16, 0);   /* dst x */
                put16(q + 18, (u16)y0);
                q[20] = 0;          /* left pad */
                q[21] = (u8)x->depth;
                if (!write_all(x->fd, q, 24) ||
                    !write_all(x->fd, wd->fallback + (size_t)y0 * w * 4, nbytes)) {
                        free(q);
                        return false;
                }
                free(q);
        }
        return true;
}

/* --------------------------------------------------------------------------
 * input + events
 * ------------------------------------------------------------------------ */

bool x11_query_pointer(X11 *x, i32 *rx, i32 *ry, u32 *mask)
{
        u8 q[8];
        memset(q, 0, sizeof q);
        q[0] = 38;   /* QueryPointer */
        put16(q + 2, 2);
        put32(q + 4, x->root);
        if (!send_req(x, q, 8))
                return false;
        u8 *rep = read_reply(x, false);
        if (!rep)
                return false;
        if (rx) *rx = (i16)get16(rep + 16);
        if (ry) *ry = (i16)get16(rep + 18);
        if (mask) *mask = get16(rep + 24);
        return true;
}

bool x11_pointer(X11 *x, RPointerState *out)
{
        if (!x || !out)
                return false;
        memset(out, 0, sizeof *out);
        i32 rx = 0, ry = 0;
        u32 mask = 0;
        if (!x11_query_pointer(x, &rx, &ry, &mask)) {
                /* position unknown — but edges still need consuming */
                out->press_edge = x->press_edge;
                out->release_edge = x->release_edge;
                out->click = x->click;
                out->click_x = x->click_x;
                out->click_y = x->click_y;
                out->click_qn = x->click_qn;
                for (u32 c = 0; c < x->click_qn && c < 4; c++) {
                        out->click_q[c][0] = x->click_q[c][0];
                        out->click_q[c][1] = x->click_q[c][1];
                }
                out->clicks = x->clicks;
                out->pressed = x->ppressed;
                x->press_edge = x->release_edge = x->click = false;
                x->click_qn = 0;
                return false;
        }
        out->valid = true;
        out->x = rx;
        out->y = ry;
        out->pressed = x->ppressed || (mask & ((1u << 8) | (1u << 9) | (1u << 10))) != 0;
        out->press_edge = x->press_edge;
        out->release_edge = x->release_edge;
        out->click = x->click;
        out->click_x = x->click_x;
        out->click_y = x->click_y;
        out->click_qn = x->click_qn;
        for (u32 c = 0; c < x->click_qn && c < 4; c++) {
                out->click_q[c][0] = x->click_q[c][0];
                out->click_q[c][1] = x->click_q[c][1];
        }
        out->clicks = x->clicks;
        /* consume-on-read */
        x->press_edge = false;
        x->release_edge = false;
        x->click = false;
        x->click_qn = 0;
        return true;
}

void x11_wait_frame(X11 *x, f64 seconds)
{
        if (!x || seconds <= 0)
                return;
        f64 deadline = rev_monotonic() + seconds;
        for (;;) {
                f64 rem = deadline - rev_monotonic();
                if (rem <= 0)
                        return;
                fd_set rf;
                FD_ZERO(&rf);
                FD_SET(x->fd, &rf);
                struct timeval tv;
                f64 slice = rem < 0.016 ? rem : 0.016;
                tv.tv_sec = (long)slice;
                tv.tv_usec = (long)((slice - (f64)tv.tv_sec) * 1e6);
                int r = select(x->fd + 1, &rf, NULL, NULL, &tv);
                if (r > 0) {
                        x11_pump_events(x);    /* events keep flowing while waiting */
                        continue;
                }
                if (rev_monotonic() >= deadline)
                        return;
        }
}

bool x11_win_geometry(X11 *x, int win, RRect *geom)
{
        if (!x || win < 0 || win >= x->nwins)
                return false;
        XWin *w = &x->wins[win];
        if (geom) {
                geom->x = 0;
                geom->y = 0;
                geom->w = (i32)w->w;
                geom->h = (i32)w->h;
        }
        return true;
}

void x11_pump_events(X11 *x)
{
        /* drain our buffered queue + anything available on the socket */
        for (;;) {
                u8 ev[32];
                if (x->evq_n > 0) {
                        memcpy(ev, x->evq[--x->evq_n], 32);
                } else {
                        fd_set rf;
                        struct timeval tv = { 0, 0 };
                        FD_ZERO(&rf);
                        FD_SET(x->fd, &rf);
                        if (select(x->fd + 1, &rf, NULL, NULL, &tv) <= 0)
                                break;
                        if (!read_all(x->fd, ev, 32))
                                break;
                }

                u8 code = ev[0];
                if (code == 22) {   /* ConfigureNotify */
                        u32 wid = get32(ev + 12);   /* event(8) window(12) */
                        u32 nw = get16(ev + 24), nh = get16(ev + 26);
                        for (int i = 0; i < x->nwins; i++) {
                                if (x->wins[i].wid == wid && (nw != x->wins[i].w || nh != x->wins[i].h)) {
                                        LOGI("x11: window %d resized %ux%u -> %ux%u",
                                             i, x->wins[i].w, x->wins[i].h, nw, nh);
                                        x->wins[i].w = nw;
                                        x->wins[i].h = nh;
                                }
                        }
                } else if (code == 4 || code == 5) {
                        /* ButtonPress / ButtonRelease — root coords at
                         * ev[20]/ev[22] (u16). Latched into edge state that
                         * x11_pointer() consumes: a full click between two
                         * polls still reports a click edge. */
                        bool down = code == 4;
                        i32 rx = (i16)get16(ev + 20);
                        i32 ry = (i16)get16(ev + 22);
                        if (down) {
                                x->ppressed = true;
                                x->press_edge = true;
                                x->click_x = (f64)rx;
                                x->click_y = (f64)ry;
                                x->click_pending = true;
                        } else {
                                x->ppressed = false;
                                x->release_edge = true;
                                if (x->click_pending) {
                                        x->click = true;
                                        x->clicks++;
                                        if (x->click_qn < 4) {
                                                x->click_q[x->click_qn][0] = x->click_x;
                                                x->click_q[x->click_qn][1] = x->click_y;
                                                x->click_qn++;
                                        } else {
                                                for (u32 c = 0; c < 3; c++) {
                                                        x->click_q[c][0] = x->click_q[c + 1][0];
                                                        x->click_q[c][1] = x->click_q[c + 1][1];
                                                }
                                                x->click_q[3][0] = x->click_x;
                                                x->click_q[3][1] = x->click_y;
                                        }
                                }
                                x->click_pending = false;
                        }
                }
                /* Expose etc. are no-ops: we repaint continuously or on demand */
        }
}

/* --------------------------------------------------------------------------
 * screenshot (GetImage)
 * ------------------------------------------------------------------------ */

u32 *x11_screenshot(X11 *x, int win, u32 *w, u32 *h)
{
        if (!x || win < 0 || win >= x->nwins)
                return NULL;
        XWin *wd = &x->wins[win];

        u8 q[20];
        memset(q, 0, sizeof q);
        q[0] = 73;          /* GetImage */
        q[1] = 2;           /* ZPixmap */
        put16(q + 2, 5);
        put32(q + 4, wd->wid);
        put16(q + 8, 0);    /* src x */
        put16(q + 10, 0);   /* src y */
        put16(q + 12, (u16)wd->w);
        put16(q + 14, (u16)wd->h);
        put32(q + 16, 0xFFFFFFFFu);   /* plane mask */
        if (!send_req(x, q, 20))
                return NULL;
        u8 *rep = read_reply(x, true);
        if (!rep || rep[1] == 0)
                return NULL;

        size_t n = (size_t)wd->w * wd->h;
        u32 *out = (u32 *)malloc(n * 4);
        if (!out)
                return NULL;

        const u32 *src = (const u32 *)(rep + 32);
        for (size_t i = 0; i < n; i++) {
                u32 p = src[i];
                u32 r = (p >> x->rshift) & (0xFFu >> (8 - 8 + x->rloss)) << x->rloss & 0xff;
                /* generic: extract each channel via mask */
                u32 rm = x->rmask, gm = x->gmask, bm = x->bmask;
                u32 rv = (p & rm) >> x->rshift;
                u32 gv = (p & gm) >> x->gshift;
                u32 bv = (p & bm) >> x->bshift;
                if (x->rloss) rv = rv * 255 / ((1u << (8 - x->rloss)) - 1);
                if (x->gloss) gv = gv * 255 / ((1u << (8 - x->gloss)) - 1);
                if (x->bloss) bv = bv * 255 / ((1u << (8 - x->bloss)) - 1);
                (void)r;
                out[i] = 0xFFu << 24 | bv << 16 | gv << 8 | rv;
        }
        *w = wd->w;
        *h = wd->h;
        return out;
}

/* --------------------------------------------------------------------------
 * GPU support accessors
 * ------------------------------------------------------------------------ */

u32 x11_window_xid(X11 *x, int win)
{
        if (!x || win < 0 || win >= x->nwins)
                return 0;
        return x->wins[win].wid;
}

/* GetWindowAttributes (opcode 3) — returns the window's visual.
 * Reply: visual u32 @8. Returns 0 on failure. */
u32 x11_win_visual(X11 *x, int win)
{
        if (!x || win < 0 || win >= x->nwins)
                return 0;
        u8 q[8];
        memset(q, 0, sizeof q);
        q[0] = 3;                /* GetWindowAttributes */
        put16(q + 2, 2);
        put32(q + 4, x->wins[win].wid);
        if (!send_req(x, q, 8))
                return 0;
        u8 *rep = read_reply(x, false);
        if (!rep || rep[0] != 1)
                return 0;
        return get32(rep + 8);
}
