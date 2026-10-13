#define _GNU_SOURCE
/* ============================================================================
 * Reverie Engine — wayland.c
 * Hand-written Wayland wire-protocol client. Zero libwayland linkage.
 *
 * Object model: wl_display is object 1; wl_registry is created via
 * get_registry; globals are advertised as (name, interface, version) events
 * and instantiated with wl_registry.bind (which carries the interface
 * string + version on the wire). Windows are zwlr_layer_surface_v1
 * BACKGROUND surfaces anchored to all edges of an output; pixels travel
 * through wl_shm memfd pools as ARGB8888 (premultiplied, per spec).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "wayland.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"
#include "platform.h"
#include "util.h"

#define WL_RBUF_CAP 16384
#define WL_WBUF_CAP 8192
#define WL_MAX_OUTPUTS 8
#define WL_MAX_BUFS 2

/* object kinds (client-side registry of ids) */
enum {
        WK_REGISTRY = 1,
        WK_CALLBACK,
        WK_COMPOSITOR,
        WK_SURFACE,
        WK_SHM,
        WK_POOL,
        WK_BUFFER,
        WK_SEAT,
        WK_POINTER,
        WK_OUTPUT,
        WK_LAYER_SHELL,
        WK_LAYER_SURFACE,
        WK_FRAME_CB,     /* wl_surface.frame callback (win >= 0)   */
};

typedef struct {
        u32 id;
        int kind;
        /* window/surface associations */
        int win;    /* for SURFACE/LAYER_SURFACE/BUFFER objects */
        int out;    /* for OUTPUT objects */
} WLObject;

typedef struct {
        u32 obj;
        i32 lx, ly;        /* logical position in global space      */
        i32 pw, ph;        /* physical pixel mode size              */
        i32 refresh_mhz;   /* mode refresh (mHz; 0 = unreported)    */
        i32 scale;
        bool have_mode, have_geom, done;
        int win;           /* window index on this output, -1 none  */
} WLOutput;

typedef struct {
        u32 surface;
        u32 layer_surface;
        u32 pool;
        u32 buf[WL_MAX_BUFS];
        bool buf_free[WL_MAX_BUFS];
        int pool_fd;
        void *pool_mem;
        size_t pool_size;
        u32 w, h;          /* logical size  */
        u32 pw, ph;        /* pixel size    */
        i32 scale;
        int out;
        bool configured;
        bool acked;
        bool closed;
        int cur;
        u32 last_serial;
        u32 frame_cb;      /* pending wl_surface.frame callback id, 0 none */
        bool frame_ready;  /* callback fired since last wait            */
} WLWin;

struct WLDisplay {
        int fd;
        u8 rbuf[WL_RBUF_CAP];
        size_t rlen, rpos;
        u8 wbuf[WL_WBUF_CAP];
        size_t wlen;
        u32 next_id;

        bool fatal;
        bool close_requested;
        char wl_error[160];    /* text of the last wl_display.error, if any */
        u32 comp_version;      /* wl_compositor version we bound */

        WLObject objs[160];
        int nobjs;

        u32 registry;
        u32 compositor;
        u32 shm;
        u32 seat;
        u32 pointer;
        u32 layer_shell;
        bool got_pointer_caps;

        WLOutput outputs[WL_MAX_OUTPUTS];
        int noutputs;

        WLWin wins[WL_MAX_OUTPUTS];
        int nwins;

        /* pointer state — deltas ACCUMULATE between wl_pointer() reads (the
         * old code overwrote them per motion event, collapsing a whole drag
         * into the last event's delta); button transitions are latched so a
         * click between two polls still reports a click edge. */
        f64 px, py, pdx, pdy;
        bool pvalid, ppressed, plasted;
        bool press_edge, release_edge, click, click_pending;
        f64 click_x, click_y;
        u32 clicks;
        int ptr_win;

        u64 drops;         /* presents dropped (both slots held)   */
};

/* --------------------------------------------------------------------------
 * wire helpers
 * ------------------------------------------------------------------------ */

static u32 wl_obj_new(WLDisplay *w, int kind, int win, int out)
{
        u32 id = w->next_id++;
        if (w->nobjs < (int)(sizeof w->objs / sizeof w->objs[0])) {
                w->objs[w->nobjs].id = id;
                w->objs[w->nobjs].kind = kind;
                w->objs[w->nobjs].win = win;
                w->objs[w->nobjs].out = out;
                w->nobjs++;
        }
        return id;
}

static WLObject *wl_obj_find(WLDisplay *w, u32 id)
{
        for (int i = 0; i < w->nobjs; i++)
                if (w->objs[i].id == id)
                        return &w->objs[i];
        return NULL;
}

static void wl_obj_del(WLDisplay *w, u32 id)
{
        for (int i = 0; i < w->nobjs; i++) {
                if (w->objs[i].id == id) {
                        w->objs[i] = w->objs[w->nobjs - 1];
                        w->nobjs--;
                        return;
                }
        }
}

static bool wl_flush(WLDisplay *w)
{
        size_t off = 0;
        while (off < w->wlen) {
                ssize_t k = write(w->fd, w->wbuf + off, w->wlen - off);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                struct pollfd p = { w->fd, POLLOUT, 0 };
                                poll(&p, 1, 100);
                                continue;
                        }
                        return false;
                }
                off += (size_t)k;
        }
        w->wlen = 0;
        return true;
}

/* message builder: header + payload room check */
static bool wl_begin_req(WLDisplay *w, u32 obj, u32 opcode, size_t payload)
{
        size_t total = 8 + payload;
        if (getenv("REV_WL_TRACE"))
                fprintf(stderr, "[wl-trace] REQ obj=%u op=%u size=%zu\n", obj, opcode, total);
        if (total > WL_WBUF_CAP) {
                if (!wl_flush(w))
                        return false;
        }
        if (w->wlen + total > WL_WBUF_CAP) {
                if (!wl_flush(w))
                        return false;
        }
        u8 *p = w->wbuf + w->wlen;
        u32 sz = (u32)total;
        memcpy(p, &obj, 4);
        /* wire: second word = (size << 16) | opcode  — size in the HIGH 16
         * bits, opcode in the LOW 16. Verified byte-exact against a live
         * weston/libwayland roundtrip (see tests + docs/BACKENDS.md). */
        u32 op = ((u32)sz << 16) | (opcode & 0xffffu);
        memcpy(p + 4, &op, 4);
        w->wlen += 8;
        return true;
}

static void wl_put_u32(WLDisplay *w, u32 v)
{
        if (w->wlen + 4 > WL_WBUF_CAP)
                return;
        memcpy(w->wbuf + w->wlen, &v, 4);
        w->wlen += 4;
}

static void wl_put_i32(WLDisplay *w, i32 v)
{
        wl_put_u32(w, (u32)v);
}

static void wl_put_str(WLDisplay *w, const char *s)
{
        size_t n = strlen(s) + 1;
        wl_put_u32(w, (u32)n);
        size_t pad = (4 - (n & 3)) & 3;
        while (n + pad > (size_t)(WL_WBUF_CAP - w->wlen) && w->wlen > 0)
                wl_flush(w);
        memcpy(w->wbuf + w->wlen, s, n);
        w->wlen += n;
        memset(w->wbuf + w->wlen, 0, pad);
        w->wlen += pad;
}

/* wl_shm.create_pool(new_id, fd, size) — the fd rides as SCM_RIGHTS */
static bool wl_create_pool(WLDisplay *w, u32 new_id, int fd, i32 size)
{
        if (!wl_flush(w))
                return false;
        u8 msg[16];
        u32 obj = w->shm;
        u32 sz = 16;
        u32 op = ((u32)sz << 16) | 0u;    /* opcode 0, size 16 */
        memcpy(msg, &obj, 4);
        memcpy(msg + 4, &op, 4);
        memcpy(msg + 8, &new_id, 4);
        memcpy(msg + 12, &size, 4);

        char cbuf[CMSG_SPACE(sizeof(int))];
        struct iovec iov = { msg, sizeof msg };
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof cbuf;
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));

        ssize_t k = sendmsg(w->fd, &mh, MSG_NOSIGNAL);
        return k == (ssize_t)sizeof msg;
}

/* ---- request helpers (typed) ---------------------------------------------- */

static bool req_get_registry(WLDisplay *w)
{
        if (!wl_begin_req(w, 1, 1, 4))    /* wl_display.get_registry = request 1 */
                return false;
        wl_put_u32(w, 2);    /* registry id = 2 */
        wl_obj_new(w, WK_REGISTRY, -1, -1);
        w->registry = 2;
        return wl_flush(w);
}

static bool req_sync(WLDisplay *w, u32 *cb_id_out)
{
        u32 id = wl_obj_new(w, WK_CALLBACK, -1, -1);
        if (!wl_begin_req(w, 1, 0, 4))    /* wl_display.sync = request 0 */
                return false;
        wl_put_u32(w, id);
        if (cb_id_out)
                *cb_id_out = id;
        return wl_flush(w);
}

static bool req_bind(WLDisplay *w, u32 name, const char *iface, u32 version, u32 *new_id_out)
{
        u32 id = wl_obj_new(w, WK_CALLBACK /*fixed below*/, -1, -1);
        /* fix kind by interface name after we know it — simpler: caller passes kind */
        (void)0;
        /* compute payload: name(4) + str(4+len+pad) + version(4) + new_id(4) */
        size_t n = strlen(iface) + 1;
        size_t pad = (4 - (n & 3)) & 3;
        size_t payload = 4 + 4 + n + pad + 4 + 4;
        if (!wl_begin_req(w, w->registry, 0, payload))
                return false;
        wl_put_u32(w, name);
        wl_put_str(w, iface);
        wl_put_u32(w, version);
        wl_put_u32(w, id);
        *new_id_out = id;
        return true;
}

/* bind with an explicit object kind (fixes the placeholder kind) */
static bool req_bind_typed(WLDisplay *w, u32 name, const char *iface, u32 version,
                           int kind, int win, int out, u32 *new_id_out)
{
        u32 id;
        if (!req_bind(w, name, iface, version, &id))
                return false;
        /* retype the just-allocated object */
        WLObject *o = wl_obj_find(w, id);
        if (o)
                o->kind = kind;
        if (win >= 0 && o)
                o->win = win;
        if (out >= 0 && o)
                o->out = out;
        *new_id_out = id;
        return true;
}

/* --------------------------------------------------------------------------
 * event parsing
 * ------------------------------------------------------------------------ */

static u32 get32(const u8 *p)
{
        u32 v;
        memcpy(&v, p, 4);
        return v;
}

static i32 gi32(const u8 *p)
{
        return (i32)get32(p);
}

static f64 gfix(const u8 *p)
{
        return (f64)gi32(p) / 256.0;
}

/* parses one event message at rbuf+rpos; returns bytes consumed or 0 if
 * incomplete, -1 on protocol violation.
 *
 * Wire format (verified byte-exact against live weston + libwayland 1.23,
 * see scripts/e2e-wayland.sh and tests/test_wayland.c):
 *   word 0: object id
 *   word 1: (size << 16) | opcode   — size INCLUDING the 8-byte header
 * The same layout is used for client requests (wl_begin_req) and for
 * compositor events, in both directions of the stream. */
static i64 wl_parse_event(WLDisplay *w, const u8 *p, size_t avail)
{
        if (avail < 8)
                return 0;
        u32 obj = get32(p);
        u32 opword = get32(p + 4);
        u32 size = opword >> 16;
        u32 opcode = opword & 0xffff;
        if (size < 8 || (size & 3) || size > WL_RBUF_CAP) {
                if (getenv("REV_WL_TRACE"))
                        fprintf(stderr, "[wl-trace] BAD MSG obj=%u op=%u size=%u avail=%zu bytes=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                                obj, opcode, size, avail,
                                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                return -1;
        }
        if (avail < size)
                return 0;
        const u8 *a = p + 8;

        /* wl_display itself is object 1 — events land there without a
         * client-side object entry. */
        if (obj == 1) {
                if (opcode == 0 && size >= 28) {
                        /* wl_display.error(object_id, code, message) */
                        u32 bad_obj = get32(a);
                        u32 code = get32(a + 4);
                        u32 slen = get32(a + 8);
                        char msg[96] = { 0 };
                        if (slen >= 1 && slen < 96)
                                memcpy(msg, a + 12, slen - 1);
                        snprintf(w->wl_error, sizeof w->wl_error,
                                 "compositor error on object %u code %u: %s",
                                 bad_obj, code, msg);
                        LOGW("wayland: %s", w->wl_error);
                        w->fatal = true;
                }
                /* opcode 1 = delete_id: our ids are never reused, so the
                 * server-side retirement is a no-op for us. */
                return (i64)size;
        }

        WLObject *o = wl_obj_find(w, obj);

        if (getenv("REV_WL_TRACE")) {
                char desc[96] = "";
                if (o) {
                        if (o->kind == WK_REGISTRY && opcode == 0) {
                                u32 slen = get32(a + 4);
                                char ifn[64] = { 0 };
                                if (slen >= 1 && slen <= 63)
                                        memcpy(ifn, a + 8, slen - 1);
                                snprintf(desc, sizeof desc, "global name=%u iface=%s v%u",
                                         get32(a), ifn, get32(a + 8 + ((slen + 3) & ~3u)));
                        } else {
                                snprintf(desc, sizeof desc, "kind=%d", o->kind);
                        }
                } else {
                        snprintf(desc, sizeof desc, "UNKNOWN-OBJ");
                }
                fprintf(stderr, "[wl-trace] EV obj=%u op=%u size=%u %s\n", obj, opcode, size, desc);
        }
        switch (o ? o->kind : 0) {
        case WK_CALLBACK:
        case WK_FRAME_CB:
                if (opcode == 0) {
                        /* wl_callback.done — for a frame callback, latch the
                         * per-window readiness the pacing loop sleeps on */
                        if (o->kind == WK_FRAME_CB && o->win >= 0 && o->win < w->nwins)
                                w->wins[o->win].frame_ready = true;
                        wl_obj_del(w, obj);    /* done */
                }
                break;

        case WK_REGISTRY: {
                if (opcode == 0) {
                        /* global(name u32, interface string, version u32) */
                        if (size < 16)
                                break;
                        u32 name = get32(a);
                        u32 slen = get32(a + 4);
                        if (slen == 0 || slen > 64)
                                break;
                        u32 voff = 8 + ((slen + 3) & ~3u);
                        if (voff + 4 > size)
                                break;
                        char iface[64];
                        u32 copy = slen < 64 ? slen : 63;
                        memcpy(iface, a + 8, copy);
                        iface[copy] = 0;
                        u32 version = get32(a + voff);
                        extern void wl_note_global(WLDisplay * w, u32 name, const char *iface, u32 version);
                        wl_note_global(w, name, iface, version);
                }
                break;
        }

        case WK_OUTPUT: {
                WLOutput *out = (o->out >= 0 && o->out < w->noutputs) ? &w->outputs[o->out] : NULL;
                if (!out)
                        break;
                if (opcode == 0) {        /* geometry */
                        out->lx = gi32(a);
                        out->ly = gi32(a + 4);
                        out->have_geom = true;
                } else if (opcode == 1) { /* mode */
                        u32 flags = get32(a);
                        out->pw = gi32(a + 4);
                        out->ph = gi32(a + 8);
                        out->refresh_mhz = gi32(a + 12);
                        if (flags & 1)         /* current mode */
                                out->have_mode = true;
                } else if (opcode == 2) { /* done */
                        out->done = true;
                } else if (opcode == 3) { /* scale */
                        out->scale = gi32(a);
                        if (out->scale < 1)
                                out->scale = 1;
                }
                break;
        }

        case WK_POINTER: {
                if (opcode == 0) {        /* enter(serial, surface, sx, sy) */
                        u32 surf = get32(a + 4);
                        f64 sx = gfix(a + 8), sy = gfix(a + 12);
                        WLObject *so = wl_obj_find(w, surf);
                        w->ptr_win = (so && so->kind == WK_SURFACE) ? so->win : -1;
                        if (w->ptr_win >= 0 && w->ptr_win < w->nwins) {
                                WLOutput *out = &w->outputs[w->wins[w->ptr_win].out];
                                w->px = out->lx + sx;
                                w->py = out->ly + sy;
                                w->pvalid = true;
                                if (!w->plasted) {
                                        w->pdx = 0;
                                        w->pdy = 0;
                                }
                                /* the enter position is a valid delta baseline:
                                 * motion after enter accumulates from it (the
                                 * jump TO the enter position is not a drag and
                                 * never counts) */
                                w->plasted = true;
                        }
                } else if (opcode == 1) { /* leave */
                        w->ptr_win = -1;
                } else if (opcode == 2) { /* motion(time, sx, sy) */
                        f64 sx = gfix(a + 4), sy = gfix(a + 8);
                        if (w->ptr_win >= 0 && w->ptr_win < w->nwins) {
                                WLOutput *out = &w->outputs[w->wins[w->ptr_win].out];
                                f64 nx = out->lx + sx, ny = out->ly + sy;
                                if (w->plasted) {
                                        /* accumulate: a drag spanning several motion
                                         * events between two polls must deliver the
                                         * TOTAL delta, not the last event's */
                                        w->pdx += nx - w->px;
                                        w->pdy += ny - w->py;
                                }
                                w->px = nx;
                                w->py = ny;
                                w->pvalid = true;
                                w->plasted = true;
                        }
                } else if (opcode == 3) { /* button(serial, time, button, state) */
                        u32 state = get32(a + 12);
                        if (state == 1) {
                                w->ppressed = true;
                                w->press_edge = true;
                                w->click_x = w->px;   /* position at press */
                                w->click_y = w->py;
                                w->click_pending = true;
                        } else {
                                w->ppressed = false;
                                w->release_edge = true;
                                if (w->click_pending) {
                                        w->click = true;
                                        w->clicks++;
                                }
                                w->click_pending = false;
                        }
                }
                break;
        }

        case WK_LAYER_SURFACE: {
                WLWin *win = (o->win >= 0 && o->win < w->nwins) ? &w->wins[o->win] : NULL;
                if (!win)
                        break;
                if (opcode == 0) {        /* configure(serial, w, h) */
                        win->last_serial = get32(a);
                        u32 cw = get32(a + 4), ch = get32(a + 8);
                        if (cw && ch) {
                                win->w = cw;
                                win->h = ch;
                                /* the compositor's assignment is authoritative —
                                 * buffers below are sized from it */
                                i32 s = win->scale < 1 ? 1 : (i32)win->scale;
                                win->pw = cw * (u32)s;
                                win->ph = ch * (u32)s;
                        }
                        win->configured = true;
                } else if (opcode == 1) { /* closed */
                        win->closed = true;
                        w->close_requested = true;
                }
                break;
        }

        case WK_BUFFER: {
                WLWin *win = (o->win >= 0 && o->win < w->nwins) ? &w->wins[o->win] : NULL;
                if (win && opcode == 0) {    /* release */
                        for (int i = 0; i < WL_MAX_BUFS; i++)
                                if (win->buf[i] == obj)
                                        win->buf_free[i] = true;
                }
                break;
        }

        default:
                break;
        }

        return (i64)size;
}

/* --------------------------------------------------------------------------
 * read/dispatch loop
 * ------------------------------------------------------------------------ */

static void wl_dispatch_pending(WLDisplay *w)
{
        for (;;) {
                /* parse buffered events first */
                while (w->rpos + 8 <= w->rlen) {
                        i64 n = wl_parse_event(w, w->rbuf + w->rpos, w->rlen - w->rpos);
                        if (n == 0)
                                break;
                        if (n < 0) {
                                LOGW("wayland: protocol parse error");
                                w->fatal = true;
                                return;
                        }
                        w->rpos += (size_t)n;
                }
                if (w->rpos == w->rlen) {
                        w->rpos = w->rlen = 0;
                } else if (w->rpos > 0) {
                        memmove(w->rbuf, w->rbuf + w->rpos, w->rlen - w->rpos);
                        w->rlen -= w->rpos;
                        w->rpos = 0;
                }

                struct pollfd p = { w->fd, POLLIN, 0 };
                if (poll(&p, 1, 0) <= 0)
                        return;
                ssize_t k = read(w->fd, w->rbuf + w->rlen, WL_RBUF_CAP - w->rlen);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        return;
                }
                if (k == 0) {
                        w->fatal = true;
                        return;
                }
                w->rlen += (size_t)k;
        }
}

/* blocks until `cond` turns true or timeout (ms) expires */

/* --------------------------------------------------------------------------
 * globals bookkeeping (the registry events arrive before we have bound)
 * ------------------------------------------------------------------------ */

/* 0.1.2 field bug: a KWin/labwc-class stacking compositor advertises ~57
 * globals; the fixed 48-entry table silently dropped zwlr_layer_shell_v1
 * (global #52) and wl_output (#57) — layer shell was then mis-reported as
 * missing and the engine bogusly fell back to XWayland. Real compositors
 * vary wildly in global count, so the table now grows on demand. */
typedef struct {
        u32 name;
        char iface[64];
        u32 version;
} WLGlobal;

static WLGlobal *g_globals;
static int g_nglobals, g_globcap;

void wl_note_global(WLDisplay *w, u32 name, const char *iface, u32 version)
{
        (void)w;
        if (g_nglobals == g_globcap) {
                int ncap = g_globcap ? g_globcap * 2 : 64;
                WLGlobal *ng = (WLGlobal *)realloc(g_globals, (size_t)ncap * sizeof *ng);
                if (!ng) {
                        static bool warned;
                        if (!warned) {
                                warned = true;
                                LOGW("wayland: global table OOM at %d entries, dropping %s",
                                     g_nglobals, iface);
                        }
                        return;
                }
                g_globals = ng;
                g_globcap = ncap;
        }
        g_globals[g_nglobals].name = name;
        snprintf(g_globals[g_nglobals].iface, sizeof g_globals->iface, "%s", iface);
        g_globals[g_nglobals].version = version;
        g_nglobals++;
}

static void wl_free_globals(void)
{
        free(g_globals);
        g_globals = NULL;
        g_globcap = 0;
        g_nglobals = 0;
}

static const WLGlobal *find_global(const char *iface)
{
        for (int i = 0; i < g_nglobals; i++)
                if (strcmp(g_globals[i].iface, iface) == 0)
                        return &g_globals[i];
        return NULL;
}

/* --------------------------------------------------------------------------
 * connection
 * ------------------------------------------------------------------------ */

WLDisplay *wl_connect(char *err, size_t errcap)
{
        const char *dir = getenv("XDG_RUNTIME_DIR");
        const char *name = getenv("WAYLAND_DISPLAY");
        if (!dir || !*dir) {
                snprintf(err, errcap, "wayland: XDG_RUNTIME_DIR is not set");
                return NULL;
        }
        if (!name || !*name)
                name = "wayland-0";

        char path[512];
        if (strchr(name, '/'))
                snprintf(path, sizeof path, "%s", name);
        else
                snprintf(path, sizeof path, "%s/%s", dir, name);

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
                snprintf(err, errcap, "wayland: socket() failed");
                return NULL;
        }
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        if (strlen(path) >= sizeof sa.sun_path) {
                snprintf(err, errcap, "wayland: socket path too long");
                close(fd);
                return NULL;
        }
        strcpy(sa.sun_path, path);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
                snprintf(err, errcap, "wayland: cannot connect to %s", path);
                close(fd);
                return NULL;
        }

        WLDisplay *w = (WLDisplay *)calloc(1, sizeof *w);
        w->fd = fd;
        w->next_id = 2;    /* registry gets id 2, matches the hardcoded wire value */
        wl_free_globals();

        if (!req_get_registry(w)) {
                snprintf(err, errcap, "wayland: get_registry failed");
                wl_disconnect(w);
                return NULL;
        }

        /* roundtrip: collect globals */
        u32 cb;
        if (!req_sync(w, &cb)) {
                snprintf(err, errcap, "wayland: sync failed");
                wl_disconnect(w);
                return NULL;
        }
        {
                bool done = false;
                /* wait until the sync callback is destroyed */
                for (int spin = 0; spin < 100 && !done && !w->fatal; spin++) {
                        wl_dispatch_pending(w);
                        done = wl_obj_find(w, cb) == NULL;
                        if (!done) {
                                struct pollfd p = { w->fd, POLLIN, 0 };
                                if (poll(&p, 1, 100) <= 0 && spin > 20)
                                        break;
                        }
                }
                if (!done) {
                        if (w->wl_error[0])
                                snprintf(err, errcap, "wayland: registry roundtrip failed (%s)",
                                         w->wl_error);
                        else
                                snprintf(err, errcap, "wayland: registry roundtrip timed out");
                        wl_disconnect(w);
                        return NULL;
                }
        }

        /* required globals */
        const WLGlobal *g_comp = find_global("wl_compositor");
        const WLGlobal *g_shm = find_global("wl_shm");
        const WLGlobal *g_shell = find_global("zwlr_layer_shell_v1");
        if (!g_comp || !g_shm || !g_shell) {
                snprintf(err, errcap,
                         "wayland: compositor lacks required globals (%s%s%s) — %d globals parsed",
                         !g_comp ? "wl_compositor " : "",
                         !g_shm ? "wl_shm " : "",
                         !g_shell ? "zwlr_layer_shell_v1 (needed for wallpaper layer; GNOME/Mutter does not support it)" : "",
                         g_nglobals);
                wl_disconnect(w);
                return NULL;
        }

        /* wl_compositor version: damage_buffer (surface op 9) needs v4 */
        w->comp_version = g_comp->version < 6 ? g_comp->version : 6;
        u32 bind_comp_v = w->comp_version;
        if (!req_bind_typed(w, g_comp->name, "wl_compositor", bind_comp_v,
                            WK_COMPOSITOR, -1, -1, &w->compositor) ||
            !req_bind_typed(w, g_shm->name, "wl_shm", g_shm->version < 1 ? 1 : g_shm->version,
                            WK_SHM, -1, -1, &w->shm) ||
            !req_bind_typed(w, g_shell->name, "zwlr_layer_shell_v1",
                            g_shell->version < 1 ? 1 : (g_shell->version > 4 ? 4 : g_shell->version),
                            WK_LAYER_SHELL, -1, -1, &w->layer_shell)) {
                snprintf(err, errcap, "wayland: bind failed");
                wl_disconnect(w);
                return NULL;
        }
        if (!wl_flush(w)) {
                snprintf(err, errcap, "wayland: flush failed");
                wl_disconnect(w);
                return NULL;
        }

        /* seat (optional but needed for interaction) */
        const WLGlobal *g_seat = find_global("wl_seat");
        if (g_seat) {
                if (!req_bind_typed(w, g_seat->name, "wl_seat", g_seat->version < 5 ? g_seat->version : 5,
                                    WK_SEAT, -1, -1, &w->seat))
                        w->seat = 0;
        }

        /* outputs */
        for (int i = 0; i < g_nglobals && w->noutputs < WL_MAX_OUTPUTS; i++) {
                if (strcmp(g_globals[i].iface, "wl_output") != 0)
                        continue;
                if (w->noutputs >= WL_MAX_OUTPUTS)
                        break;
                u32 outid;
                if (!req_bind_typed(w, g_globals[i].name, "wl_output",
                                    g_globals[i].version < 4 ? g_globals[i].version : 4,
                                    WK_OUTPUT, -1, w->noutputs, &outid))
                        continue;
                w->outputs[w->noutputs].obj = outid;
                w->outputs[w->noutputs].scale = 1;
                w->outputs[w->noutputs].win = -1;
                w->noutputs++;
        }
        wl_flush(w);

        /* pointer */
        if (w->seat) {
                w->pointer = wl_obj_new(w, WK_POINTER, -1, -1);
                if (!wl_begin_req(w, w->seat, 0, 4))    /* get_pointer */
                        w->pointer = 0;
                else
                        wl_put_u32(w, w->pointer);
                wl_flush(w);
        }

        /* roundtrip for outputs + seat caps */
        req_sync(w, &cb);
        {
                for (int spin = 0; spin < 100; spin++) {
                        wl_dispatch_pending(w);
                        if (wl_obj_find(w, cb) == NULL)
                                break;
                        struct pollfd p = { w->fd, POLLIN, 0 };
                        if (poll(&p, 1, 100) <= 0 && spin > 20)
                                break;
                }
        }

        /* outputs without mode info get a fallback */
        for (int i = 0; i < w->noutputs; i++) {
                if (!w->outputs[i].have_mode) {
                        w->outputs[i].pw = 1280;
                        w->outputs[i].ph = 720;
                }
        }
        if (w->noutputs == 0) {
                snprintf(err, errcap, "wayland: no outputs");
                wl_disconnect(w);
                return NULL;
        }
        return w;
}

/* --------------------------------------------------------------------------
 * monitors / windows
 * ------------------------------------------------------------------------ */

int wl_monitors(WLDisplay *w, RRect *out, int max)
{
        int n = w->noutputs < max ? w->noutputs : max;
        for (int i = 0; i < n; i++) {
                i32 s = w->outputs[i].scale < 1 ? 1 : w->outputs[i].scale;
                out[i].x = w->outputs[i].lx;
                out[i].y = w->outputs[i].ly;
                out[i].w = w->outputs[i].pw / s;
                out[i].h = w->outputs[i].ph / s;
        }
        return n;
}

f64 wl_monitor_scale(WLDisplay *w, int mon)
{
        if (mon < 0 || mon >= w->noutputs)
                return 1.0;
        return (f64)(w->outputs[mon].scale < 1 ? 1 : w->outputs[mon].scale);
}

int wl_monitor_refresh(WLDisplay *w, int mon)
{
        if (!w || mon < 0 || mon >= w->noutputs)
                return 0;
        i32 mhz = w->outputs[mon].refresh_mhz;
        /* mHz -> Hz, rounded; sanity gate against junk values */
        if (mhz < 10000 || mhz > 1000000)
                return 0;
        return (int)((mhz + 500) / 1000);
}

int wl_add_window(WLDisplay *w, int mon)
{
        if (mon < 0 || mon >= w->noutputs || w->nwins >= WL_MAX_OUTPUTS)
                return -1;
        if (w->outputs[mon].win >= 0)
                return w->outputs[mon].win;    /* already has a window */

        int wi = w->nwins;
        WLWin *win = &w->wins[wi];
        i32 scale = w->outputs[mon].scale < 1 ? 1 : w->outputs[mon].scale;
        win->out = mon;
        win->scale = scale;
        win->w = (u32)(w->outputs[mon].pw / scale);
        win->h = (u32)(w->outputs[mon].ph / scale);
        win->pw = w->outputs[mon].pw;
        win->ph = w->outputs[mon].ph;
        win->cur = 0;
        for (int i = 0; i < WL_MAX_BUFS; i++)
                win->buf_free[i] = true;
        w->outputs[mon].win = wi;

        win->surface = wl_obj_new(w, WK_SURFACE, wi, -1);
        if (!wl_begin_req(w, w->compositor, 0, 4))    /* create_surface */
                return -1;
        wl_put_u32(w, win->surface);

        win->layer_surface = wl_obj_new(w, WK_LAYER_SURFACE, wi, -1);
        /* zwlr_layer_shell_v1.get_layer_surface — request opcode 0:
         *   get_layer_surface(new_id, surface, output, layer, namespace)
         * layer enum: background=0 bottom=1 top=2 overlay=3 (we want 0).
         * Opcode 1 is destroy (since v3) — sending this request as opcode 1
         * (the 0.1.2 bug) made KWin kill the connection with
         * "invalid object <new_id>" and the layer surface never configured. */
        {
                const char *ns = "reverie";
                size_t nsl = strlen(ns) + 1;
                size_t payload = 4 + 4 + 4 + 4 + 4 + nsl + ((4 - (nsl & 3)) & 3);
                if (!wl_begin_req(w, w->layer_shell, 0, payload))
                        return -1;
                wl_put_u32(w, win->layer_surface);
                wl_put_u32(w, win->surface);
                wl_put_u32(w, w->outputs[mon].obj);
                wl_put_u32(w, 0);              /* layer: background */
                wl_put_str(w, ns);
        }
        /* zwlr_layer_surface_v1 request opcodes — REAL numbering per
         * wlr-layer-shell-unstable-v1.xml (canonical upstream, the same file
         * sway/wlroots/KWin implement):
         *   0 set_size(uu)              1 set_anchor(u)
         *   2 set_exclusive_zone(i)     3 set_margin(iiii)
         *   4 set_keyboard_interactivity(u)
         *   5 get_popup(o)              6 ack_configure(u)
         *   7 destroy                   8 set_layer (since v2)
         * set_size(0,0) + anchor(all four edges) is the canonical wallpaper
         * pattern: 0 means "let the anchors determine the size" — the
         * compositor's assignment arrives via configure(). The 0.1.5 table
         * dropped set_size and shifted the whole family by one: the anchor
         * value 15 went out as a 4-byte payload on opcode 0, which strict
         * compositors demarshal as set_size(uu) needing 8 bytes and kill the
         * connection with "invalid arguments for set_size". */
        if (!wl_begin_req(w, win->layer_surface, 0, 8))    /* set_size(0,0) */
                return -1;
        wl_put_u32(w, 0);
        wl_put_u32(w, 0);
        if (!wl_begin_req(w, win->layer_surface, 1, 4))    /* set_anchor */
                return -1;
        wl_put_u32(w, 15);                 /* top|bottom|left|right */
        if (!wl_begin_req(w, win->layer_surface, 2, 4))    /* set_exclusive_zone */
                return -1;
        wl_put_i32(w, -1);                 /* fill the whole output */
        if (!wl_begin_req(w, win->layer_surface, 4, 4))    /* set_keyboard_interactivity */
                return -1;
        wl_put_u32(w, 0);                  /* none */

        /* commit with no buffer */
        if (!wl_begin_req(w, win->surface, 6, 0))
                return -1;
        if (!wl_flush(w))
                return -1;

        /* the window must be visible to the event parser (win index) before
         * we wait for the configure event */
        w->nwins++;

        /* wait for configure */
        {
                for (int spin = 0; spin < 100 && !win->configured && !w->fatal; spin++) {
                        wl_dispatch_pending(w);
                        if (win->configured)
                                break;
                        struct pollfd p = { w->fd, POLLIN, 0 };
                        if (poll(&p, 1, 100) <= 0 && spin > 60)
                                break;
                }
        }
        if (!win->configured) {
                LOGW("wayland: layer surface did not configure");
                return -1;
        }

        /* ack_configure — opcode 6 per the real protocol (opcode 5 is
         * get_popup, opcode 7 is destroy). The 0.1.5 client acked via the
         * get_popup slot after the set_size deletion shifted the table. */
        if (!wl_begin_req(w, win->layer_surface, 6, 4))
                return -1;
        wl_put_u32(w, win->last_serial);
        win->acked = true;

        /* shm pool + buffers */
        win->pool_size = (size_t)win->pw * win->ph * 4 * WL_MAX_BUFS;
        win->pool_fd = memfd_create("reverie-wl", 0);
        if (win->pool_fd < 0) {
                LOGW("wayland: memfd_create failed");
                return -1;
        }
        if (ftruncate(win->pool_fd, (off_t)win->pool_size) < 0) {
                close(win->pool_fd);
                win->pool_fd = -1;
                return -1;
        }
        win->pool_mem = mmap(NULL, win->pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, win->pool_fd, 0);
        if (win->pool_mem == MAP_FAILED) {
                win->pool_mem = NULL;
                close(win->pool_fd);
                win->pool_fd = -1;
                return -1;
        }
        win->pool = wl_obj_new(w, WK_POOL, wi, -1);
        if (!wl_create_pool(w, win->pool, win->pool_fd, (i32)win->pool_size))
                return -1;

        for (int b = 0; b < WL_MAX_BUFS; b++) {
                win->buf[b] = wl_obj_new(w, WK_BUFFER, wi, -1);
                if (!wl_begin_req(w, win->pool, 0, 24))
                        return -1;
                wl_put_u32(w, win->buf[b]);
                wl_put_i32(w, (i32)((size_t)win->pw * win->ph * 4 * b));
                wl_put_i32(w, (i32)win->pw);
                wl_put_i32(w, (i32)win->ph);
                wl_put_i32(w, (i32)(win->pw * 4));
                wl_put_u32(w, 0);              /* WL_SHM_FORMAT_ARGB8888 */
        }
        if (!wl_flush(w))
                return -1;

        /* first (empty) attach so the surface becomes visible */
        {
                u8 *dst = (u8 *)win->pool_mem;
                memset(dst, 0, (size_t)win->pw * win->ph * 4);
                if (!wl_begin_req(w, win->surface, 1, 12))    /* attach */
                        return -1;
                wl_put_u32(w, win->buf[0]);
                wl_put_i32(w, 0);
                wl_put_i32(w, 0);
                /* damage_buffer (op 9) exists since wl_surface v4 and takes
                 * BUFFER coords (16-byte payload); older compositors get
                 * logical damage (op 2) in surface-local coords — also
                 * 16 bytes, never the 20 the 0.1.5 client sent */
                if (w->comp_version >= 4) {
                        if (!wl_begin_req(w, win->surface, 9, 16))
                                return -1;
                        wl_put_i32(w, 0);
                        wl_put_i32(w, 0);
                        wl_put_u32(w, win->pw);
                        wl_put_u32(w, win->ph);
                } else {
                        if (!wl_begin_req(w, win->surface, 2, 16))
                                return -1;
                        wl_put_i32(w, 0);
                        wl_put_i32(w, 0);
                        wl_put_u32(w, win->w);
                        wl_put_u32(w, win->h);
                }
                if (win->scale > 1 && w->comp_version >= 3) {
                        if (!wl_begin_req(w, win->surface, 8, 4))    /* set_buffer_scale */
                                return -1;
                        wl_put_i32(w, win->scale);
                }
                if (!wl_begin_req(w, win->surface, 6, 0))    /* commit */
                        return -1;
                if (!wl_flush(w))
                        return -1;
                win->buf_free[0] = false;
                win->cur = 1;
        }

        return wi;
}

/* --------------------------------------------------------------------------
 * present
 * ------------------------------------------------------------------------ */

bool wl_present(WLDisplay *w, int win_, const u32 *rgba, u32 width, u32 height)
{
        if (!w || win_ < 0 || win_ >= w->nwins)
                return false;
        WLWin *win = &w->wins[win_];
        if (win->closed || w->fatal)
                return false;
        if (width != win->pw || height != win->ph || !rgba) {
                if (rgba && (width != win->pw || height != win->ph)) {
                        static bool warned;
                        if (!warned) {
                                warned = true;
                                LOGW("wayland: frame %ux%u != configured buffer %ux%u — dropping frames",
                                     width, height, win->pw, win->ph);
                        }
                }
                return false;
        }

        /* pick a free buffer; dispatch first to collect releases */
        wl_dispatch_pending(w);
        int b = -1;
        for (int i = 0; i < WL_MAX_BUFS; i++)
                if (win->buf_free[i]) {
                        b = i;
                        break;
                }
        if (b < 0) {
                /* server still holds both buffers — drop this frame (the
                 * double-buffer cycle is the real frame limiter here) */
                w->drops++;
                return true;
        }

        /* premultiply into the shm pool slot */
        u32 *dst = (u32 *)((u8 *)win->pool_mem + (size_t)win->pw * win->ph * 4 * b);
        size_t n = (size_t)width * height;
        for (size_t i = 0; i < n; i++) {
                u32 p = rgba[i];
                u32 a = p >> 24;
                if (a == 255) {
                        dst[i] = p;
                } else if (a == 0) {
                        dst[i] = 0;
                } else {
                        u32 r = (p & 0xff), g = (p >> 8) & 0xff, bl = (p >> 16) & 0xff;
                        dst[i] = (a << 24) |
                                 (((bl * a + 127) / 255) << 16) |
                                 (((g * a + 127) / 255) << 8) |
                                 ((r * a + 127) / 255);
                }
        }

        if (!wl_begin_req(w, win->surface, 1, 12))    /* attach */
                return false;
        wl_put_u32(w, win->buf[b]);
        wl_put_i32(w, 0);
        wl_put_i32(w, 0);
        if (w->comp_version >= 4) {
                if (!wl_begin_req(w, win->surface, 9, 16))
                        return false;   /* damage_buffer: buffer coords */
                wl_put_i32(w, 0);
                wl_put_i32(w, 0);
                wl_put_u32(w, width);
                wl_put_u32(w, height);
        } else {
                if (!wl_begin_req(w, win->surface, 2, 16))
                        return false;   /* damage: surface-local coords */
                wl_put_i32(w, 0);
                wl_put_i32(w, 0);
                wl_put_u32(w, win->w);
                wl_put_u32(w, win->h);
        }
        if (!wl_begin_req(w, win->surface, 6, 0))     /* commit */
                return false;

        /* wl_surface.frame (opcode 3) — request the callback the pacing loop
         * sleeps on; the compositor fires it when this frame is latched, so
         * the next present aligns with the actual repaint cycle.
         * (Opcode sanity: destroy=0 attach=1 damage=2 frame=3 ... commit=6
         * damage_buffer=9 — from wayland.xml; the "13" in an earlier draft
         * of this code was wrong and the strict fake compositor rejects it.) */
        if (!win->frame_cb) {
                u32 cb = wl_obj_new(w, WK_FRAME_CB, win_, -1);
                if (wl_begin_req(w, win->surface, 3, 4)) {
                        wl_put_u32(w, cb);
                        win->frame_cb = cb;
                } else {
                        wl_obj_del(w, cb);
                }
        }

        if (!wl_flush(w))
                return false;
        win->buf_free[b] = false;
        win->cur = b;
        return true;
}

/* --------------------------------------------------------------------------
 * state
 * ------------------------------------------------------------------------ */

void wl_pump(WLDisplay *w)
{
        if (w)
                wl_dispatch_pending(w);
}

bool wl_pointer(WLDisplay *w, RPointerState *out)
{
        if (!w || !out)
                return false;
        out->valid = w->pvalid;
        out->pressed = w->ppressed;
        out->x = w->px;
        out->y = w->py;
        out->dx = w->pdx;
        out->dy = w->pdy;
        out->press_edge = w->press_edge;
        out->release_edge = w->release_edge;
        out->click = w->click;
        out->click_x = w->click_x;
        out->click_y = w->click_y;
        out->clicks = w->clicks;
        if (!w->pvalid)
                return false;
        /* consume-on-read: each delta batch / button edge is delivered exactly
         * once — no stale repeats, no swallowed clicks */
        w->pdx = 0;
        w->pdy = 0;
        w->press_edge = false;
        w->release_edge = false;
        w->click = false;
        return true;
}

bool wl_wait_frame(WLDisplay *w, f64 seconds)
{
        if (!w || w->fatal)
                return false;
        if (seconds < 0)
                seconds = 0;
        f64 deadline = rev_monotonic() + seconds;

        for (;;) {
                /* pending events first (non-blocking) */
                wl_dispatch_pending(w);

                bool any = false;
                for (int i = 0; i < w->nwins; i++) {
                        if (w->wins[i].frame_cb && w->wins[i].frame_ready) {
                                w->wins[i].frame_cb = 0;    /* consumed */
                                w->wins[i].frame_ready = false;
                                any = true;
                        }
                }
                if (any || w->fatal)
                        return any;

                f64 rem = deadline - rev_monotonic();
                if (rem <= 0)
                        return false;
                int ms = (int)(rem * 1000.0);
                if (ms < 1)
                        ms = 1;
                if (ms > 16)
                        ms = 16;    /* slice: stay responsive to non-callback data */
                struct pollfd p = { w->fd, POLLIN, 0 };
                poll(&p, 1, ms);
        }
}

u64 wl_dropped_frames(WLDisplay *w)
{
        return w ? w->drops : 0;
}

bool wl_win_geometry(WLDisplay *w, int win_, RRect *geom)
{
        if (!w || win_ < 0 || win_ >= w->nwins)
                return false;
        WLWin *win = &w->wins[win_];
        geom->x = w->outputs[win->out].lx;
        geom->y = w->outputs[win->out].ly;
        geom->w = (i32)win->w;
        geom->h = (i32)win->h;
        return true;
}

void wl_sync(WLDisplay *w)
{
        if (!w)
                return;
        u32 cb;
        if (!req_sync(w, &cb))
                return;
        for (int spin = 0; spin < 100; spin++) {
                wl_dispatch_pending(w);
                if (wl_obj_find(w, cb) == NULL)
                        return;
                struct pollfd p = { w->fd, POLLIN, 0 };
                if (poll(&p, 1, 100) <= 0 && spin > 20)
                        return;
        }
}

bool wl_should_close(WLDisplay *w)
{
        return w && w->close_requested;
}

bool wl_fatal(WLDisplay *w)
{
        return w && w->fatal;
}

void wl_disconnect(WLDisplay *w)
{
        if (!w)
                return;
        for (int i = 0; i < w->nwins; i++) {
                if (w->wins[i].pool_mem)
                        munmap(w->wins[i].pool_mem, w->wins[i].pool_size);
                if (w->wins[i].pool_fd >= 0)
                        close(w->wins[i].pool_fd);
        }
        if (w->fd >= 0)
                close(w->fd);
        free(w);
        wl_free_globals();
}
