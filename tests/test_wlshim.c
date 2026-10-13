/* ============================================================================
 * Reverie Engine — tests/test_wlshim.c
 * E2E test of the GPU-path Wayland shim (wlshim.c) against a STRICT fake
 * compositor — with the REAL libwayland-client doing the marshalling.
 *
 * Why this exists: wlshim hands hand-written wl_interface / wl_message
 * metadata tables to a genuine libwayland (dlopen'd). Two families of ABI
 * bugs lived in those tables and this call site, invisible to every existing
 * test because the raw wire client (wayland.c) has its own encoder:
 *
 *   1. wl_message was declared with TWO fields (signature, types) instead of
 *      THREE (name FIRST, then signature, then types) — a two-field table
 *      shifts every slot: libwayland reads the signature as the name, NULL
 *      as the signature, and dereferences past the struct for types.
 *   2. wl_proxy_marshal_flags was called without the NULL placeholder the
 *      new_id vararg slot consumes (every later argument shifted by one),
 *      with flags=1 (WL_MARSHAL_FLAG_DESTROY — destroys the shell proxy!)
 *      instead of 0, and with version 0 instead of the shell's bound
 *      version.
 *
 * Neither the fake compositor nor the raw-client tests can catch these: the
 * bug is in what LIBWAYLAND produces from our tables. So this test drives
 * the shim end-to-end — connect, registry, binds, layer-surface setup,
 * configure/ack, pointer events, closed — with real libwayland on the client
 * side and the strict validator on the server side. If the metadata tables
 * are wrong, real libwayland itself misbehaves and the validator catches it.
 *
 * The fake-compositor core below mirrors tests/test_wayland.c (standalone
 * binaries; keep the two validators in sync when the protocol net changes).
 *
 * Skips gracefully when no real libwayland-client can be dlopened.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "rev_types.h"
#include "wlshim.h"

/* ---- tiny server helpers (mirrored from tests/test_wayland.c) -------------- */

static int srv_fd = -1;
static int cli_fd = -1;
static char runtime_dir[64];
static char sock_name[32];
static u8 msg[512];
static size_t n;

static u32 s_get32(const u8 *p)
{
        u32 v;
        memcpy(&v, p, 4);
        return v;
}

static void s_put32(u8 *p, u32 v)
{
        memcpy(p, &v, 4);
}

/* server -> client event: object, opcode, payload.
 * REAL wire format: word1 = (size << 16) | opcode, size INCLUDING header. */
static void s_send(u32 obj, u32 opcode, const u8 *payload, size_t plen)
{
        u8 hdr[8];
        u32 size = (u32)(8 + plen);
        s_put32(hdr, obj);
        s_put32(hdr + 4, ((u32)size << 16) | (opcode & 0xffffu));
        if (write(cli_fd, hdr, 8) != 8)
                exit(2);
        if (plen && write(cli_fd, payload, plen) != (ssize_t)plen)
                exit(2);
}

static void s_send_str_event(u32 obj, u32 opcode, u32 a, const char *s, u32 b)
{
        /* event with (u32, string, u32) args — wl_registry.global layout */
        u32 sl = (u32)strlen(s) + 1;
        size_t pad = (4 - (sl & 3)) & 3;
        size_t plen = 4 + 4 + sl + pad + 4;
        u8 *p = (u8 *)calloc(1, plen);
        s_put32(p, a);
        s_put32(p + 4, sl);
        memcpy(p + 8, s, sl);
        s_put32(p + 8 + sl + pad, b);
        s_send(obj, opcode, p, plen);
        free(p);
}

/* read exactly ONE client message (framed). */
static size_t s_read_msg(u8 *buf, size_t cap, int *out_fd)
{
        if (out_fd)
                *out_fd = -1;
        size_t got = 0;
        while (got < 8) {
                struct iovec iov = { buf + got, 8 - got };
                char cbuf[CMSG_SPACE(sizeof(int))];
                struct msghdr mh;
                memset(&mh, 0, sizeof mh);
                mh.msg_iov = &iov;
                mh.msg_iovlen = 1;
                mh.msg_control = cbuf;
                mh.msg_controllen = sizeof cbuf;
                ssize_t k = recvmsg(cli_fd, &mh, 0);
                if (k <= 0)
                        return 0;
                if (out_fd && *out_fd == -1) {
                        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
                        if (cm && cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
                                memcpy(out_fd, CMSG_DATA(cm), sizeof(int));
                }
                got += (size_t)k;
        }
        u32 size = s_get32(buf + 4) >> 16;   /* size in the HIGH 16 bits */
        if (size < 8 || size > cap)
                return 0;
        while (got < size) {
                ssize_t k = read(cli_fd, buf + got, size - got);
                if (k <= 0)
                        return 0;
                got += (size_t)k;
        }
        return size;
}

static u32 msg_opcode(const u8 *m)
{
        return s_get32(m + 4) & 0xffff;    /* opcode in the LOW 16 bits */
}

static u32 msg_size(const u8 *m)
{
        return s_get32(m + 4) >> 16;
}

#define OBJ_DISPLAY 1
#define OBJ_REGISTRY 2

/* ---- strict request validator (mirrored from tests/test_wayland.c) ----------
 * Every request must target a KNOWN object with a KNOWN opcode and the exact
 * wire size; new_ids are registered as they are created; destructor requests
 * retire their object. Violations post a wl_display.error exactly like a real
 * compositor. Real libwayland sits on the other end of this socket: if our
 * hand-written interface metadata is malformed, the requests it marshals fail
 * right here. */
enum {
        T_DISPLAY, T_REGISTRY, T_COMPOSITOR, T_SURFACE, T_SHM, T_POOL, T_BUFFER,
        T_SEAT, T_POINTER, T_OUTPUT, T_SHELL, T_LAYER_SURFACE,
};

typedef struct { u32 id; u8 type; } SObj;
static SObj sobjs[96];
static int nsobjs;
static char srv_err[128];
static bool srv_error_posted;

static void s_reset_objects(void)
{
        nsobjs = 0;
        srv_err[0] = 0;
        srv_error_posted = false;
}

static int s_find_idx(u32 id)
{
        for (int i = 0; i < nsobjs; i++)
                if (sobjs[i].id == id)
                        return i;
        return -1;
}

static void s_reg(u32 id, u8 type)
{
        if (id && s_find_idx(id) < 0 && nsobjs < (int)(sizeof sobjs / sizeof sobjs[0])) {
                sobjs[nsobjs].id = id;
                sobjs[nsobjs].type = type;
                nsobjs++;
        }
}

static void s_unreg(u32 id)
{
        int i = s_find_idx(id);
        if (i >= 0) {
                sobjs[i] = sobjs[nsobjs - 1];
                nsobjs--;
        }
}

static u8 s_type_of(u32 obj)
{
        if (obj == OBJ_DISPLAY)
                return T_DISPLAY;
        if (obj == OBJ_REGISTRY)
                return T_REGISTRY;
        int i = s_find_idx(obj);
        return i >= 0 ? sobjs[i].type : 0xff;
}

static void s_post_error(u32 obj, u32 code, const char *text)
{
        u32 ml = (u32)strlen(text) + 1;
        u32 pad = (4 - (ml & 3)) & 3;
        size_t plen = 4 + 4 + 4 + ml + pad;
        u8 *p = (u8 *)calloc(1, plen);
        if (!p)
                return;
        s_put32(p, obj);
        s_put32(p + 4, code);
        s_put32(p + 8, ml);
        memcpy(p + 12, text, ml);
        s_send(OBJ_DISPLAY, 0, p, plen);
        free(p);
        srv_error_posted = true;
}

static bool s_check(const u8 *m, size_t len)
{
        u32 obj = s_get32(m);
        u32 op = msg_opcode(m);
        u32 size = msg_size(m);
        u8 t = s_type_of(obj);
        if (t == 0xff) {
                snprintf(srv_err, sizeof srv_err, "invalid object %u", obj);
                s_post_error(obj, 0, srv_err);
                return false;
        }
        u32 want = 0;
        int nidoff = -1;
        u8 nid_type = 0xff;
        bool is_bind = false, varsz = false, destructor = false;
        switch (t) {
        case T_DISPLAY:
                if (op == 0 || op == 1)
                        want = 12;
                break;
        case T_REGISTRY:
                if (op == 0)
                        is_bind = true;
                break;
        case T_COMPOSITOR:
                if (op == 0) { want = 12; nidoff = 8; nid_type = T_SURFACE; }
                break;
        case T_SHM:
                if (op == 0) { want = 16; nidoff = 8; nid_type = T_POOL; }
                break;
        case T_POOL:
                if (op == 0) { want = 32; nidoff = 8; nid_type = T_BUFFER; }
                else if (op == 1) { want = 8; destructor = true; }
                else if (op == 2) want = 12;
                break;
        case T_BUFFER:
                if (op == 0) { want = 8; destructor = true; }
                break;
        case T_SURFACE:
                if (op == 0) { want = 8; destructor = true; }
                else if (op == 1) want = 20;                          /* attach */
                else if (op == 2 || op == 9) want = 24;               /* damage(_buffer) */
                else if (op == 3) want = 12;                          /* frame (cb) */
                else if (op == 4 || op == 5 || op == 7 || op == 8) want = 12;
                else if (op == 6) want = 8;                           /* commit */
                break;
        case T_SEAT:
                if (op <= 2) { want = 12; nidoff = 8; nid_type = T_POINTER; }
                else if (op == 3) { want = 8; destructor = true; }    /* release */
                break;
        case T_POINTER:
                if (op == 0) want = 24;                                 /* set_cursor */
                else if (op == 1) { want = 8; destructor = true; }    /* release */
                break;
        case T_OUTPUT:
                if (op == 0) { want = 8; destructor = true; }         /* release (v3) */
                break;
        case T_SHELL:                     /* zwlr_layer_shell_v1 */
                if (op == 0)
                        varsz = true;    /* get_layer_surface */
                else if (op == 1) { want = 8; destructor = true; }    /* destroy */
                break;
        case T_LAYER_SURFACE:             /* zwlr_layer_surface_v1 */
                if (op == 0 || op == 1 || op == 3 || op == 5) want = 12;
                else if (op == 2) want = 24;                          /* set_margin */
                else if (op == 4) want = 12;                          /* get_popup */
                else if (op == 6) { want = 8; destructor = true; }    /* destroy */
                else if (op == 7) want = 8;                           /* set_layer */
                break;
        }
        if (is_bind) {
                if (size < 24 || len < size) {
                        snprintf(srv_err, sizeof srv_err, "test: bind bad size %u", size);
                        s_post_error(obj, 1, srv_err);
                        return false;
                }
                u32 slen = s_get32(m + 12);
                if (slen == 0 || slen > 64 || 20 + ((slen + 3) & ~3u) > size) {
                        snprintf(srv_err, sizeof srv_err, "test: bind interface string malformed");
                        s_post_error(obj, 1, srv_err);
                        return false;
                }
                char iface[64] = { 0 };
                memcpy(iface, m + 16, slen - 1);
                u32 new_id = s_get32(m + 20 + ((slen + 3) & ~3u));
                u8 bt = T_SURFACE;
                if (!strcmp(iface, "wl_compositor"))         bt = T_COMPOSITOR;
                else if (!strcmp(iface, "wl_shm"))           bt = T_SHM;
                else if (!strcmp(iface, "wl_seat"))          bt = T_SEAT;
                else if (!strcmp(iface, "wl_output"))        bt = T_OUTPUT;
                else if (!strcmp(iface, "zwlr_layer_shell_v1")) bt = T_SHELL;
                s_reg(new_id, bt);
                return true;
        }
        if (varsz) {
                /* get_layer_surface(new_id, surface, output, layer, namespace) */
                if (len < size || size < 32 || (size & 3)) {
                        snprintf(srv_err, sizeof srv_err, "test: get_layer_surface bad size %u", size);
                        s_post_error(obj, 1, srv_err);
                        return false;
                }
                u32 nid = s_get32(m + 8);
                u32 surf = s_get32(m + 12);
                u32 outp = s_get32(m + 16);
                u32 slen = s_get32(m + 24);
                if (s_type_of(surf) != T_SURFACE || (outp && s_type_of(outp) != T_OUTPUT)) {
                        snprintf(srv_err, sizeof srv_err,
                                 "test: get_layer_surface bad object refs");
                        s_post_error(obj, 0, srv_err);
                        return false;
                }
                if (slen == 0 || 28 + ((slen + 3) & ~3u) > size) {
                        snprintf(srv_err, sizeof srv_err,
                                 "test: get_layer_surface namespace malformed");
                        s_post_error(obj, 1, srv_err);
                        return false;
                }
                s_reg(nid, T_LAYER_SURFACE);
                return true;
        }
        if (want == 0) {
                snprintf(srv_err, sizeof srv_err, "test: unknown opcode %u on object %u",
                         op, obj);
                s_post_error(obj, 1, srv_err);
                return false;
        }
        if (size != want || len != size) {
                snprintf(srv_err, sizeof srv_err, "test: object %u opcode %u size %u != %u",
                         obj, op, size, want);
                s_post_error(obj, 1, srv_err);
                return false;
        }
        if (nidoff >= 0 && nid_type != 0xff)
                s_reg(s_get32(m + nidoff), nid_type);
        if (destructor)
                s_unreg(obj);
        return true;
}

/* ---- server scenario state --------------------------------------------------- */

static u32 id_compositor, id_shell, id_seat, id_output, id_surface, id_layer_surface, id_pointer;
static bool saw_layer0, saw_ns_reverie, saw_anchor15, saw_zone_neg1, saw_interact0, saw_ack77;
static bool closed_sent, pointer_sent;

static int failures = 0;
#define CHECK(cond, msg) \
        do { \
                if (!(cond)) { \
                        printf("  FAIL: %s\n", msg); \
                        failures++; \
                } else { \
                        printf("  ok: %s\n", msg); \
                } \
        } while (0)

/* reply to wl_display.sync: callback done(serial) + delete_id (like a real
 * compositor retiring the callback object). REAL libwayland demarshals the
 * done event against its signature "u" — the 4-byte serial payload is
 * mandatory or the client flags a protocol error and kills the connection. */
static void reply_sync(u32 cb)
{
        u8 s[4];
        s_put32(s, 0);                      /* serial */
        s_send(cb, 0, s, 4);                /* callback done(serial) */
        u8 d[4];
        s_put32(d, cb);
        s_send(OBJ_DISPLAY, 1, d, 4);      /* delete_id */
}

/* wl_output info so the shim fills geometry/scale: logical 400x300 at scale 2
 * (physical mode 800x600) — geometry, mode(current), scale, done (v4 order). */
static void send_output_events(void)
{
        {
                /* geometry(x, y, pw, ph, subpixel, make, model, transform) */
                u8 p[40];
                s_put32(p, 0); s_put32(p + 4, 0);
                s_put32(p + 8, 600); s_put32(p + 12, 340);
                s_put32(p + 16, 1);        /* subpixel: unknown */
                s_put32(p + 20, 2);        /* make: "T\0"    */
                p[24] = 'T'; p[25] = 0; p[26] = 0; p[27] = 0;
                s_put32(p + 28, 2);        /* model: "T\0"   */
                p[32] = 'T'; p[33] = 0; p[34] = 0; p[35] = 0;
                s_put32(p + 36, 0);        /* transform: normal */
                s_send(id_output, 0, p, 40);
        }
        {
                u8 p[16];
                s_put32(p, 1);             /* flags: current */
                s_put32(p + 4, 800);       /* width  */
                s_put32(p + 8, 600);       /* height */
                s_put32(p + 12, 60000);    /* refresh */
                s_send(id_output, 1, p, 16);
        }
        {
                u8 p[4];
                s_put32(p, 2);             /* scale factor 2 */
                s_send(id_output, 3, p, 4);
        }
        s_send(id_output, 2, NULL, 0);     /* done */
}

/* pointer events exercising the shim's listener wiring through real
 * libwayland dispatch: enter(surface, 100,150), motion(120,180), button(press) */
static void send_pointer_events(void)
{
        {
                u8 p[16];
                s_put32(p, 5);             /* serial */
                s_put32(p + 4, id_surface);
                s_put32(p + 8, 100 << 8);  /* sx = 100.0 fixed */
                s_put32(p + 12, 150 << 8);
                s_send(id_pointer, 0, p, 16);   /* enter */
        }
        {
                u8 m[12];
                s_put32(m, 999);           /* time */
                s_put32(m + 4, 120 << 8);
                s_put32(m + 8, 180 << 8);
                s_send(id_pointer, 2, m, 12);   /* motion */
        }
        {
                u8 b[16];
                s_put32(b, 7);             /* serial */
                s_put32(b + 4, 1000);      /* time */
                s_put32(b + 8, 272);       /* left button */
                s_put32(b + 12, 1);        /* pressed */
                s_send(id_pointer, 3, b, 16);   /* button */
        }
        pointer_sent = true;
}

/* ---- the server thread: the whole conversation ------------------------------- */
static void *server(void *arg)
{
        (void)arg;
        cli_fd = accept(srv_fd, NULL, NULL);
        if (cli_fd < 0)
                return NULL;

        /* 1. get_registry — must be the first message real libwayland sends */
        n = s_read_msg(msg, sizeof msg, NULL);
        if (n < 12)
                return NULL;
        CHECK(s_get32(msg) == OBJ_DISPLAY && msg_opcode(msg) == 1 && msg_size(msg) == 12,
              "get_registry addressed to wl_display opcode 1");
        if (!s_check(msg, n))
                return NULL;

        /* 2. sync #1 — reply with globals + callback retirement. The shim
         * collects globals during THIS roundtrip and binds afterwards. */
        n = s_read_msg(msg, sizeof msg, NULL);
        if (n < 12)
                return NULL;
        CHECK(s_get32(msg) == OBJ_DISPLAY && msg_opcode(msg) == 0 && msg_size(msg) == 12,
              "sync (roundtrip #1) framed correctly");
        u32 sync_cb = s_get32(msg + 8);
        if (!s_check(msg, n))
                return NULL;
        s_send_str_event(OBJ_REGISTRY, 0, 10, "wl_compositor", 4);
        s_send_str_event(OBJ_REGISTRY, 0, 11, "wl_shm", 1);
        s_send_str_event(OBJ_REGISTRY, 0, 12, "wl_seat", 5);
        s_send_str_event(OBJ_REGISTRY, 0, 13, "wl_output", 4);
        s_send_str_event(OBJ_REGISTRY, 0, 14, "zwlr_layer_shell_v1", 4);
        reply_sync(sync_cb);

        /* 3. everything else in one loop: binds (learn ids from the wire),
         * syncs (reply), surface + layer-surface setup, configure, ack. */
        for (;;) {
                n = s_read_msg(msg, sizeof msg, NULL);
                if (n < 8)
                        break;                   /* client disconnected */
                if (!s_check(msg, n))
                        break;                   /* violation posted; stop */
                u32 obj = s_get32(msg);
                u32 op = msg_opcode(msg);
                u8 t = s_type_of(obj);

                if (t == T_DISPLAY && op == 0) {
                        /* a later roundtrip: after the pointer events have been
                         * consumed, retire the layer surface (closed) first */
                        if (pointer_sent && !closed_sent) {
                                s_send(id_layer_surface, 1, NULL, 0);
                                closed_sent = true;
                        }
                        reply_sync(s_get32(msg + 8));
                        continue;
                }
                if (t == T_REGISTRY && op == 0) {
                        /* bind(name, interface, version, new_id) */
                        u32 slen = s_get32(msg + 12);
                        char iface[64] = { 0 };
                        memcpy(iface, msg + 16, slen > 63 ? 63 : slen - 1);
                        u32 padded = ((slen + 3) & ~3u);
                        u32 version = s_get32(msg + 16 + padded);
                        u32 new_id = s_get32(msg + 20 + padded);
                        if (!strcmp(iface, "wl_compositor")) {
                                id_compositor = new_id;
                                CHECK(version == 4, "wl_compositor bound at v4");
                        } else if (!strcmp(iface, "zwlr_layer_shell_v1")) {
                                id_shell = new_id;
                                CHECK(version == 4,
                                      "zwlr_layer_shell_v1 bound at v4 (metadata name read by libwayland)");
                        } else if (!strcmp(iface, "wl_seat")) {
                                id_seat = new_id;
                                /* capabilities: pointer + keyboard */
                                u8 c[4];
                                s_put32(c, 3);
                                s_send(id_seat, 0, c, 4);
                        } else if (!strcmp(iface, "wl_output")) {
                                id_output = new_id;
                                send_output_events();
                        }
                        continue;
                }
                if (t == T_COMPOSITOR && op == 0) {
                        id_surface = s_get32(msg + 8);
                        continue;
                }
                if (t == T_SEAT && op == 0) {
                        id_pointer = s_get32(msg + 8);   /* get_pointer */
                        continue;
                }
                if (t == T_SHELL && op == 0) {
                        /* get_layer_surface(new_id, surface, output, layer, ns)
                         * wire offsets: nid@8 surf@12 outp@16 layer@20 len@24 */
                        id_layer_surface = s_get32(msg + 8);
                        CHECK(s_get32(msg + 20) == 0,
                              "get_layer_surface layer == BACKGROUND(0)");
                        saw_layer0 = s_get32(msg + 20) == 0;
                        u32 slen = s_get32(msg + 24);
                        bool ns_ok = slen == 8 && memcmp(msg + 28, "reverie", 7) == 0 &&
                                     msg[35] == 0;
                        CHECK(ns_ok, "get_layer_surface namespace == \"reverie\"");
                        saw_ns_reverie = ns_ok;
                        CHECK(s_type_of(s_get32(msg + 12)) == T_SURFACE,
                              "get_layer_surface references the real wl_surface");
                        continue;
                }
                if (t == T_LAYER_SURFACE) {
                        if (op == 0) {
                                bool ok = n == 12 && s_get32(msg + 8) == 15;
                                CHECK(ok, "set_anchor(all edges=15) — opcode 0");
                                saw_anchor15 = ok;
                        } else if (op == 1) {
                                bool ok = n == 12 && (i32)s_get32(msg + 8) == -1;
                                CHECK(ok, "set_exclusive_zone(-1) — opcode 1");
                                saw_zone_neg1 = ok;
                        } else if (op == 3) {
                                bool ok = n == 12 && s_get32(msg + 8) == 0;
                                CHECK(ok, "set_keyboard_interactivity(none) — opcode 3");
                                saw_interact0 = ok;
                        } else if (op == 5) {
                                bool ok = s_get32(msg + 8) == 77;
                                CHECK(ok, "ack_configure(serial=77) — opcode 5");
                                saw_ack77 = ok;
                                send_pointer_events();
                        }
                        continue;
                }
                if (t == T_SURFACE && op == 6) {
                        /* initial commit — an all-edges layer surface is sized
                         * by the compositor: configure(serial, w, h) */
                        u8 p[12];
                        s_put32(p, 77);
                        s_put32(p + 4, 400);
                        s_put32(p + 8, 300);
                        s_send(id_layer_surface, 0, p, 12);
                        continue;
                }
        }
        return NULL;
}

/* ---- test --------------------------------------------------------------------- */

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);   /* failures stay visible if we die */
        alarm(30);                          /* hang guard: real libwayland may
                                             * block in dispatch if our metadata
                                             * tables confuse it — die loudly
                                             * instead of hanging the suite */

        /* 0. pick a REAL libwayland-client: an explicit REV_WL_LIB pin, the
         * default soname (the production path), or the local test stack.
         * No library anywhere -> SKIP (e.g. minimal build boxes). */
        const char *pin = getenv("REV_WL_LIB");
        const char *stack = "../gpulibs/root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0";
        void *probe = NULL;
        if (pin && *pin) {
                probe = dlopen(pin, RTLD_NOW | RTLD_LOCAL);
        } else {
                probe = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
                if (!probe) {
                        probe = dlopen(stack, RTLD_NOW | RTLD_LOCAL);
                        if (probe)
                                setenv("REV_WL_LIB", stack, 1);
                }
        }
        if (!probe) {
                printf("wlshim: SKIP (no real libwayland-client to dlopen)\n");
                return 0;
        }
        dlclose(probe);
        setenv("REV_WAYLAND_GPU", "1", 1);

        /* 1. runtime dir + listening socket, exactly like a compositor */
        snprintf(runtime_dir, sizeof runtime_dir, "/tmp/revwlshim.%d", (int)getpid());
        mkdir(runtime_dir, 0700);
        snprintf(sock_name, sizeof sock_name, "testwlshim-0");

        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        if (strlen(runtime_dir) + strlen(sock_name) + 1 >= sizeof sa.sun_path) {
                printf("runtime path too long\n");
                return 1;
        }
        int plen = snprintf(sa.sun_path, sizeof sa.sun_path - 1, "%s/%s", runtime_dir, sock_name);
        if (plen < 0 || (size_t)plen >= sizeof sa.sun_path - 1) {
                printf("runtime path too long\n");
                return 1;
        }
        unlink(sa.sun_path);
        srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (bind(srv_fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
                printf("bind failed: %s\n", strerror(errno));
                return 1;
        }
        listen(srv_fd, 1);

        setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
        setenv("WAYLAND_DISPLAY", sock_name, 1);
        s_reset_objects();

        pthread_t th;
        pthread_create(&th, NULL, server, NULL);

        /* 2. activation + init: real libwayland connects, marshals the
         * registry roundtrip, and binds through OUR interface metadata. */
        CHECK(wlshim_active(), "wlshim activated (REV_WAYLAND_GPU=1)");

        RPlatform plat;
        memset(&plat, 0, sizeof plat);
        char err[256] = { 0 };
        bool inited = wlshim_platform_init(&plat, err, sizeof err);
        CHECK(inited, err[0] ? err : "wlshim init: registry + binds through real libwayland");

        int win = -1;
        if (inited) {
                /* 3. monitor enumeration from the output events */
                RRect mon[8];
                int nm = wlshim_monitors(mon, 8);
                CHECK(nm == 1, "one monitor enumerated");
                CHECK(nm == 1 && mon[0].x == 0 && mon[0].y == 0 &&
                      mon[0].w == 400 && mon[0].h == 300,
                      "monitor logical geometry 400x300 (mode 800x600 @ scale 2)");
                CHECK(wlshim_monitor_scale(0) == 2.0, "monitor scale 2.0");

                /* 4. window: create_surface + get_layer_surface marshalled by
                 * real libwayland from our hand-written tables, then the
                 * anchor/zone/interact requests, commit, configure, ack. */
                RRect zero = { 0, 0, 0, 0 };
                win = wlshim_add_window(zero);
                CHECK(win == 0, "layer surface created via real libwayland marshalling");

                if (win == 0) {
                        RRect wg = { 0, 0, 0, 0 };
                        CHECK(wlshim_win_geometry(win, &wg) &&
                              wg.x == 0 && wg.y == 0 && wg.w == 400 && wg.h == 300,
                              "window geometry 400x300 after configure");
                        CHECK(wlshim_surface_handle(win) != 0,
                              "wl_surface handle exposed for EGL/Vulkan");
                        CHECK(wlshim_display_handle() != NULL,
                              "wl_display handle exposed for EGL/Vulkan");
                }

                /* 5. pointer events (sent by the server after the ack) — pump
                 * until they arrive (server thread may lag a moment). */
                RPointerState ptr;
                memset(&ptr, 0, sizeof ptr);
                for (int i = 0; i < 200 && !ptr.valid; i++) {
                        wlshim_pump(&ptr);
                        if (!ptr.valid)
                                usleep(10000);
                }
                CHECK(ptr.valid && ptr.x == 120.0 && ptr.y == 180.0,
                      "pointer at (120,180) via libwayland event dispatch");
                CHECK(ptr.pressed, "pointer pressed");

                /* 6. roundtrip: the server retires the layer surface right
                 * before replying — closed must be dispatched by then. */
                wlshim_sync();
                CHECK(wlshim_should_close(), "layer surface closed detected");
        }

        /* 7. teardown: disconnect lets the server loop exit cleanly; the
         * shutdown also unblocks accept() if init failed before connecting */
        wlshim_platform_fini(&plat);
        shutdown(srv_fd, SHUT_RDWR);
        pthread_join(th, NULL);

        /* 8. scenario flags recorded by the server (re-checked post-join so
         * they are authoritative even if the thread raced the CHECKs) */
        CHECK(saw_layer0, "layer=background(0) on the wire");
        CHECK(saw_ns_reverie, "namespace=\"reverie\" on the wire");
        CHECK(saw_anchor15, "set_anchor(all) seen");
        CHECK(saw_zone_neg1, "set_exclusive_zone(-1) seen");
        CHECK(saw_interact0, "set_keyboard_interactivity(none) seen");
        CHECK(saw_ack77, "ack_configure(serial) seen");

        /* the strict validator must have seen ZERO violations from REAL
         * libwayland working off OUR metadata tables across the whole
         * session — this is the line that failed for every wl_message /
         * marshal_flags ABI slip. */
        CHECK(!srv_error_posted, "strict validator: zero protocol violations");
        if (srv_error_posted)
                printf("    strict validator says: %s\n", srv_err);

        close(cli_fd);
        close(srv_fd);
        unlink(sa.sun_path);
        rmdir(runtime_dir);

        if (failures == 0) {
                printf("wlshim: OK\n");
                return 0;
        }
        printf("wlshim: %d failure(s)\n", failures);
        return 1;
}
