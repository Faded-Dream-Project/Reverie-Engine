/* ============================================================================
 * Reverie Engine — tests/test_wayland.c
 * E2E test of the raw Wayland wire client against a hand-written fake
 * compositor over a unix socket: handshake, registry, binds (byte-exact),
 * layer-surface configure flow, wl_shm pool with a real memfd (server
 * mmaps it and checks PIXELS), buffer attach/commit, and pointer events.
 *
 * The fake compositor is STRICT: every client request is validated (target
 * object must be known, opcode must exist for that interface, wire size must
 * match, new_ids are registered) and violations receive a real
 * wl_display.error — the same response a real compositor gives. The lenient
 * 0.1.2 fake agreed with the client's shifted zwlr_layer_surface_v1 opcodes
 * and the bug only surfaced on a real stacking compositor (KWin).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "rev_types.h"
#include "wayland.h"

/* ---- tiny server helpers --------------------------------------------------- */

static int srv_fd = -1;
static int cli_fd = -1;
static char runtime_dir[64];
static char sock_name[32];
static u8 msg[512];
static size_t n;

typedef struct {
        u32 pointer_id;
        u32 sync2;
} PhaseA;
static PhaseA A;
static volatile bool saw_size = false, saw_anchor = false, saw_zone = false, saw_interact = false;

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
 * REAL wire format, byte-pinned to a live weston/libwayland capture:
 *   word1 = (size << 16) | opcode  — size INCLUDING the 8-byte header. */
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
        /* event with (u32, string, u32) args — layout used by wl_registry.global */
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

/* read exactly ONE client message (framed): header first, then payload.
 * The fd (if any) arrives with the first byte of the message. */
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

/* the fake compositor's object ids — the client picks its own ids starting
 * at 2 (registry) then 3+ for binds; the SERVER just remembers the mapping */
#define OBJ_DISPLAY 1
#define OBJ_REGISTRY 2

/* ---- strict request validator (mimics libwayland-server) ---------------------
 * Every request must target a KNOWN object with a KNOWN opcode and the exact
 * wire size; new_ids are registered as they are created; destructor requests
 * retire their object. Violations post a wl_display.error exactly like a real
 * compositor (KWin/weston) would.
 *
 * Regression net for two field bugs, both caused by hand-written opcode
 * tables drifting from the real protocol (validated here against the
 * canonical wlr-layer-shell-unstable-v1.xml, the same file sway/wlroots/
 * KWin implement):
 *   - 0.1.2: get_layer_surface sent as opcode 1 (destroy) — KWin killed the
 *     connection with "invalid object 12".
 *   0.1.5: the fix for that bug over-corrected and DELETED set_size from
 *     the zwlr_layer_surface_v1 table, shifting every request by one. The
 *     anchor value 15 went out as a 4-byte payload on opcode 0; strict
 *     compositors demarshal opcode 0 as set_size(uu) — 8 payload bytes —
 *     and post "invalid arguments for zwlr_layer_surface_v1#N.set_size",
 *     exactly as reported from the field. Both times the lenient fake
 *     agreed with the client's wrong table (both sides were written
 *     together), so every test stayed green until a real compositor ran
 *     the client. The tables below now match the XML 1:1. */
enum {
        T_DISPLAY, T_REGISTRY, T_COMPOSITOR, T_SURFACE, T_SHM, T_POOL, T_BUFFER,
        T_SEAT, T_POINTER, T_OUTPUT, T_SHELL, T_LAYER_SURFACE,
};

typedef struct { u32 id; u8 type; } SObj;
static SObj sobjs[96];
static int nsobjs;
static char srv_err[128];    /* last validator error (empty = none seen) */
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

/* wl_display.error(object_id, code, message) — what KWin sent in the field */
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

/* validate ONE client request; false = violation (error already posted to
 * the client, exactly like a real compositor would) */
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
        u32 want = 0;           /* expected wire size, 0 = reject */
        int nidoff = -1;        /* payload offset of a new_id arg    */
        u8 nid_type = 0xff;     /* type to register for that new_id  */
        bool is_bind = false, varsz = false, destructor = false;
        switch (t) {
        case T_DISPLAY:                    /* 0 sync, 1 get_registry (callbacks not tracked) */
                if (op == 0 || op == 1)
                        want = 12;
                break;
        case T_REGISTRY:
                if (op == 0)
                        is_bind = true;    /* bind(name, iface, version, new_id) */
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
                else if (op == 2) want = 12;               /* resize */
                break;
        case T_BUFFER:
                if (op == 0) { want = 8; destructor = true; }
                break;
        case T_SURFACE:
                if (op == 0) { want = 8; destructor = true; }          /* destroy */
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
        case T_LAYER_SURFACE:             /* zwlr_layer_surface_v1 — REAL
                                          * numbering, 1:1 with the XML */
                if (op == 0) want = 16;                               /* set_size(uu) */
                else if (op == 1 || op == 2 || op == 4) want = 12;    /* set_anchor /
                                                                          * set_exclusive_zone /
                                                                          * set_keyboard_interactivity */
                else if (op == 3) want = 24;                          /* set_margin(iiii) */
                else if (op == 5) want = 12;                          /* get_popup(o) */
                else if (op == 6) want = 12;                          /* ack_configure(u) */
                else if (op == 7) { want = 8; destructor = true; }    /* destroy */
                else if (op == 8) want = 12;                          /* set_layer(u) v2 */
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

/* ---- the fake compositor's state -------------------------------------------- */
typedef struct {
        u32 compositor;   /* client id of wl_compositor          */
        u32 shm;          /* client id of wl_shm                 */
        u32 seat;         /* client id of wl_seat                */
        u32 layer_shell;  /* client id of zwlr_layer_shell_v1    */
        u32 output;       /* client id of wl_output              */
        u32 surface;      /* client id of wl_surface             */
        u32 layer_surface;
        u32 pool;
        u32 buffer[2];
} ServerState;

static ServerState S;

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

/* ---- test ------------------------------------------------------------------- */

static void *phase_a(void *arg)
{
        (void)arg;
        cli_fd = accept(srv_fd, NULL, NULL);
                if (cli_fd < 0)
                        return NULL;

                /* 1. get_registry (object 1, opcode 1, new_id 2).
                 * GOLDEN BYTE CHECK: bytes captured from a real libwayland
                 * client (weston-simple-shm over the relay) — regression-pins
                 * our wire encoding to the reference implementation. */
                n = s_read_msg(msg, sizeof msg, NULL);
                CHECK(n >= 12, "received get_registry");
                CHECK(s_get32(msg) == OBJ_DISPLAY && msg_opcode(msg) == 1,
                      "get_registry addressed to wl_display opcode 1");
                CHECK(msg_size(msg) == 12, "get_registry message size 12");
                if (n >= 8)
                        s_check(msg, n);    /* strict: object 1, opcode 1 */
                {
                        static const u8 golden_registry[12] = {
                                0x01, 0x00, 0x00, 0x00,   /* object 1       */
                                0x01, 0x00, 0x0c, 0x00,   /* (12<<16)|op 1  */
                                0x02, 0x00, 0x00, 0x00    /* new_id 2       */
                        };
                        CHECK(n == 12 && memcmp(msg, golden_registry, 12) == 0,
                              "get_registry bytes == real libwayland golden");
                }
                CHECK(s_get32(msg + 8) == OBJ_REGISTRY, "registry new_id == 2");

                /* 2. sync (object 1, opcode 0) */
                n = s_read_msg(msg, sizeof msg, NULL);
                CHECK(n >= 12, "received sync");
                if (n >= 8)
                        s_check(msg, n);
                u32 sync_cb = s_get32(msg + 8);
                CHECK(msg_opcode(msg) == 0, "sync opcode 0");
                CHECK(msg_size(msg) == 12, "sync message size 12");
                {
                        /* header captured from libwayland: 01 00 00 00 00 00 0c 00 */
                        CHECK(s_get32(msg) == 1 && msg_opcode(msg) == 0 && msg_size(msg) == 12,
                              "sync header bytes match real libwayland");
                }

                /* globals + sync callback done + delete_id (like a real
                 * compositor retires the callback id) */
                s_send_str_event(OBJ_REGISTRY, 0, 10, "wl_compositor", 4);
                s_send_str_event(OBJ_REGISTRY, 0, 11, "wl_shm", 1);
                s_send_str_event(OBJ_REGISTRY, 0, 12, "wl_seat", 5);
                s_send_str_event(OBJ_REGISTRY, 0, 13, "wl_output", 4);
                s_send_str_event(OBJ_REGISTRY, 0, 14, "zwlr_layer_shell_v1", 4);
                s_send(sync_cb, 0, NULL, 0);
                {
                        u8 d[4];
                        s_put32(d, sync_cb);
                        s_send(OBJ_DISPLAY, 1, d, 4);   /* wl_display.delete_id */
                }

                /* 3. binds */
                for (int i = 0; i < 5; i++) {
                        n = s_read_msg(msg, sizeof msg, NULL);
                        CHECK(n >= 24, "received bind");
                        if (n < 24)
                                continue;    /* client gone: skip stale-buffer parsing */
                        if (!s_check(msg, n))
                                continue;    /* violation posted; client will die */
                        u32 name = s_get32(msg + 8);
                        u32 slen = s_get32(msg + 12);
                        char iface[64] = { 0 };
                        memcpy(iface, msg + 16, slen > 63 ? 63 : slen - 1);
                        u32 padded = ((slen + 3) & ~3u);
                        u32 version = s_get32(msg + 16 + padded);
                        u32 new_id = s_get32(msg + 20 + padded);
                        fprintf(stderr, "\n");
                        printf("    bind name=%u iface=%s v%u id=%u\n", name, iface, version, new_id);
                        if (strcmp(iface, "wl_compositor") == 0) S.compositor = new_id;
                        else if (strcmp(iface, "wl_shm") == 0) S.shm = new_id;
                        else if (strcmp(iface, "zwlr_layer_shell_v1") == 0) S.layer_shell = new_id;
                        else if (strcmp(iface, "wl_seat") == 0) S.seat = new_id;
                        else if (strcmp(iface, "wl_output") == 0) S.output = new_id;
                }
                CHECK(S.compositor && S.shm && S.layer_shell && S.seat && S.output,
                      "bound compositor+shm+layer_shell+seat+output");

                /* seat capabilities: pointer present */
                {
                        u8 p[8];
                        s_put32(p, 1);
                        s_send(S.seat, 0, p, 4);
                }
                /* output geometry/mode/scale/done */
                {
                        u8 p[32];
                        memset(p, 0, sizeof p);
                        s_put32(p, 0);
                        s_put32(p + 4, 0);
                        s_put32(p + 8, 640);
                        s_put32(p + 12, 400);
                        s_send(S.output, 0, p, 24);
                        u8 m[20];
                        memset(m, 0, sizeof m);
                        s_put32(m, 1);
                        s_put32(m + 4, 800);
                        s_put32(m + 8, 600);
                        s_put32(m + 12, 60000);
                        s_send(S.output, 1, m, 16);
                        u8 sc[8];
                        s_put32(sc, 2);
                        s_send(S.output, 3, sc, 4);
                        s_send(S.output, 2, NULL, 0);
                }

                /* get_pointer THEN the final sync (client sends them in this order) */
                n = s_read_msg(msg, sizeof msg, NULL);
                CHECK(n >= 12, "received get_pointer");
                CHECK(msg_opcode(msg) == 0, "get_pointer opcode 0");
                if (n >= 8)
                        s_check(msg, n);    /* registers the pointer object */
                u32 pointer_id = s_get32(msg + 8);
                n = s_read_msg(msg, sizeof msg, NULL);
                if (n >= 8)
                        s_check(msg, n);
                u32 sync2 = s_get32(msg + 8);
                CHECK(msg_opcode(msg) == 0, "second sync");
                s_send(sync2, 0, NULL, 0);
                A.pointer_id = pointer_id;
                A.sync2 = sync2;
                return NULL;
}

/* ---- flood regression (v0.1.2 field bug) -------------------------------------
 * A stacking compositor (KWin/labwc class) advertised 57 globals; the fixed
 * 48-entry global table silently dropped zwlr_layer_shell_v1 (global #52)
 * and wl_output (#57) — layer shell was mis-reported as missing and the
 * engine bogusly fell back to XWayland. Flood the registry past the old cap
 * with the required globals at their FIELD positions and require a clean
 * connect with layer shell bound. */
static u32 flood_shell_name, flood_output_name;

static void *phase_flood(void *arg)
{
        (void)arg;
        cli_fd = accept(srv_fd, NULL, NULL);
        if (cli_fd < 0)
                return NULL;

        /* client opens with get_registry + sync (golden bytes checked below
         * in the canonical scenario; here we only need the framing) */
        n = s_read_msg(msg, sizeof msg, NULL);
        if (n >= 8)
                s_check(msg, n);
        n = s_read_msg(msg, sizeof msg, NULL);
        if (n >= 8)
                s_check(msg, n);
        u32 sync_cb = s_get32(msg + 8);

        /* 57 globals, required ones at their field positions: wl_shm #2,
         * wl_compositor #11, wl_seat #17, zwlr_layer_shell_v1 #52,
         * wl_output #57 — both shell and output sit past the old 48 cap. */
        static const struct { u32 name; const char *iface; u32 v; } req[] = {
                { 2,  "wl_shm", 2 },
                { 11, "wl_compositor", 6 },
                { 17, "wl_seat", 9 },
                { 52, "zwlr_layer_shell_v1", 4 },
                { 57, "wl_output", 4 },
        };
        char filler[64];
        for (u32 name = 1; name <= 57; name++) {
                const char *iface = NULL;
                u32 v = 1;
                for (size_t i = 0; i < sizeof req / sizeof req[0]; i++) {
                        if (req[i].name == name) {
                                iface = req[i].iface;
                                v = req[i].v;
                        }
                }
                if (!iface) {
                        if (name == 33) {
                                /* longest name from the field trace (47 chars) */
                                iface = "ext_foreign_toplevel_image_capture_source_manager_v1";
                        } else {
                                snprintf(filler, sizeof filler, "wp_filler_global_%u_v1", name);
                                iface = filler;
                        }
                }
                s_send_str_event(OBJ_REGISTRY, 0, name, iface, v);
        }
        s_send(sync_cb, 0, NULL, 0);
        {
                u8 d[4];
                s_put32(d, sync_cb);
                s_send(OBJ_DISPLAY, 1, d, 4);   /* delete_id */
        }

        /* five binds: compositor, shm, layer_shell, seat, output */
        for (int i = 0; i < 5; i++) {
                n = s_read_msg(msg, sizeof msg, NULL);
                CHECK(n >= 24, "flood: received bind");
                if (n < 24)
                        continue;    /* client gone: skip stale-buffer parsing */
                if (!s_check(msg, n))
                        continue;
                u32 name = s_get32(msg + 8);
                u32 slen = s_get32(msg + 12);
                char iface[64] = { 0 };
                memcpy(iface, msg + 16, slen > 63 ? 63 : slen - 1);
                u32 padded = ((slen + 3) & ~3u);
                u32 new_id = s_get32(msg + 20 + padded);
                if (strcmp(iface, "wl_compositor") == 0) {
                        S.compositor = new_id;
                } else if (strcmp(iface, "wl_shm") == 0) {
                        S.shm = new_id;
                } else if (strcmp(iface, "zwlr_layer_shell_v1") == 0) {
                        S.layer_shell = new_id;
                        flood_shell_name = name;
                } else if (strcmp(iface, "wl_seat") == 0) {
                        S.seat = new_id;
                } else if (strcmp(iface, "wl_output") == 0) {
                        S.output = new_id;
                        flood_output_name = name;
                }
        }
        CHECK(S.layer_shell != 0 && flood_shell_name == 52,
              "flood: layer_shell (global #52, past the old 48 cap) found + bound");
        CHECK(S.output != 0 && flood_output_name == 57,
              "flood: wl_output (global #57, past the old 48 cap) found + bound");

        /* seat capabilities: pointer present */
        {
                u8 p[8];
                s_put32(p, 1);
                s_send(S.seat, 0, p, 4);
        }
        /* output geometry/mode/scale/done */
        {
                u8 p[32];
                memset(p, 0, sizeof p);
                s_put32(p, 0);
                s_put32(p + 4, 0);
                s_put32(p + 8, 640);
                s_put32(p + 12, 400);
                s_send(S.output, 0, p, 24);
                u8 m[20];
                memset(m, 0, sizeof m);
                s_put32(m, 1);
                s_put32(m + 4, 800);
                s_put32(m + 8, 600);
                s_put32(m + 12, 60000);
                s_send(S.output, 1, m, 16);
                u8 sc[8];
                s_put32(sc, 2);
                s_send(S.output, 3, sc, 4);
                s_send(S.output, 2, NULL, 0);
        }

        /* get_pointer THEN the final sync (client order) */
        n = s_read_msg(msg, sizeof msg, NULL);
        if (n >= 8)
                s_check(msg, n);
        n = s_read_msg(msg, sizeof msg, NULL);
        if (n < 12)
                return NULL;    /* client gone before the final roundtrip */
        s_check(msg, n);
        u32 sync2 = s_get32(msg + 8);
        s_send(sync2, 0, NULL, 0);
        return NULL;
}

static void *phase_b(void *arg)
{
        (void)arg;
        n = s_read_msg(msg, sizeof msg, NULL);
        CHECK(n >= 12, "received create_surface");
        CHECK(s_get32(msg) == S.compositor && msg_opcode(msg) == 0,
              "compositor.create_surface (opcode 0)");
        S.surface = s_get32(msg + 8);
        if (n >= 8)
                s_check(msg, n);    /* registers the surface object */

        n = s_read_msg(msg, sizeof msg, NULL);
        CHECK(n >= 32, "received get_layer_surface");
        /* opcode MUST be 0: opcode 1 is destroy — the 0.1.2 client sent the
         * creation request as a destroy and KWin killed the connection. */
        CHECK(s_get32(msg) == S.layer_shell && msg_opcode(msg) == 0,
              "layer_shell.get_layer_surface (opcode 0)");
        S.layer_surface = s_get32(msg + 8);
        u32 surf_arg = s_get32(msg + 12);
        u32 output_arg = s_get32(msg + 16);
        u32 layer = s_get32(msg + 20);
        CHECK(surf_arg == S.surface, "get_layer_surface(surface) is our surface");
        CHECK(output_arg == S.output, "get_layer_surface(output) is our output");
        CHECK(layer == 0, "layer == BACKGROUND(0)");
        if (n >= 8)
                s_check(msg, n);    /* registers the layer_surface object */

        /* drain set_size / set_anchor / set_exclusive_zone /
         * set_keyboard_interactivity + commit(null) — each strictly validated
         * against the REAL protocol sizes by s_check above */
        for (int i = 0; i < 8; i++) {
                n = s_read_msg(msg, sizeof msg, NULL);
                if (n < 8)
                        break;
                if (!s_check(msg, n))
                        break;    /* violation: client connection is dead */
                u32 op = msg_opcode(msg);
                u32 obj = s_get32(msg);
                if (obj == S.layer_surface && op == 0 && n == 16 &&
                    s_get32(msg + 8) == 0 && s_get32(msg + 12) == 0)
                        saw_size = true;
                if (obj == S.layer_surface && op == 1 && n == 12 && s_get32(msg + 8) == 15)
                        saw_anchor = true;
                if (obj == S.layer_surface && op == 2 && n == 12 && (i32)s_get32(msg + 8) == -1)
                        saw_zone = true;
                if (obj == S.layer_surface && op == 4 && n == 12 && s_get32(msg + 8) == 0)
                        saw_interact = true;
                if (obj == S.surface && op == 6)
                        break;    /* commit(null) */
        }

        /* configure -> client must ack and attach a first buffer */
        {
                u8 p[16];
                s_put32(p, 77);
                s_put32(p + 4, 400);
                s_put32(p + 8, 300);
                s_send(S.layer_surface, 0, p, 12);
        }
                return NULL;
}

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);   /* failures stay visible if we die */

        /* runtime dir + socket */
        snprintf(runtime_dir, sizeof runtime_dir, "/tmp/revwltest.%d", (int)getpid());
        mkdir(runtime_dir, 0700);
        snprintf(sock_name, sizeof sock_name, "testwl-0");

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

        /* ---- flood regression run (fresh socket, fresh client state) ----- */
        {
                s_reset_objects();
                struct sockaddr_un fa;
                memset(&fa, 0, sizeof fa);
                fa.sun_family = AF_UNIX;
                if (strlen(runtime_dir) + strlen("testwl-flood") + 1 >= sizeof fa.sun_path) {
                        printf("flood runtime path too long\n");
                        return 1;
                }
                snprintf(fa.sun_path, sizeof fa.sun_path, "%s/testwl-flood", runtime_dir);
                unlink(fa.sun_path);
                int save_srv = srv_fd;
                srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (bind(srv_fd, (struct sockaddr *)&fa, sizeof fa) < 0) {
                        printf("flood bind failed: %s\n", strerror(errno));
                        return 1;
                }
                listen(srv_fd, 1);
                setenv("WAYLAND_DISPLAY", "testwl-flood", 1);

                char ferr[256] = { 0 };
                pthread_t thf;
                pthread_create(&thf, NULL, phase_flood, NULL);
                WLDisplay *fw = wl_connect(ferr, sizeof ferr);
                CHECK(fw != NULL, ferr[0] ? ferr : "flood: wl_connect with 57 globals");
                pthread_join(thf, NULL);
                wl_disconnect(fw);
                close(cli_fd);
                close(srv_fd);
                srv_fd = save_srv;
                unlink(fa.sun_path);

                /* restore env + server state for the canonical scenario */
                setenv("WAYLAND_DISPLAY", sock_name, 1);
                memset(&S, 0, sizeof S);
                flood_shell_name = flood_output_name = 0;
                s_reset_objects();    /* new connection: ids restart at 2 */
        }

        /* client connects in a forked child? No — the client is in-process:
         * connect BEFORE accept by forking accept into the parent flow. Simplest:
         * the client connects to the listening socket; we accept afterwards. */

        char err[256] = { 0 };
        WLDisplay *wl = NULL;

        /* The handshake must run concurrently with wl_connect (which blocks on
         * its registry roundtrip) — server phase A runs in a thread. */
        pthread_t th;
        pthread_create(&th, NULL, phase_a, &A);
        /* client connects (blocks until the server answers the roundtrips) */
        wl = wl_connect(err, sizeof err);
        CHECK(wl != NULL, err[0] ? err : "wl_connect");
        pthread_join(th, NULL);
        CHECK(cli_fd >= 0, "server accepted client");

        /* 5. add_window — the server side (reads + configure) must run while the
         * client blocks inside wl_add_window waiting for the configure event. */

        pthread_t thb;
        pthread_create(&thb, NULL, phase_b, NULL);
        int fd = -1;
        int win = wl_add_window(wl, 0);
        CHECK(win == 0, "window created");
        pthread_join(thb, NULL);
        CHECK(saw_size, "set_size(0,0) — opcode 0, 16-byte frame");
        CHECK(saw_anchor, "set_anchor(all edges) — opcode 1");
        CHECK(saw_zone, "set_exclusive_zone(-1) — opcode 2");
        CHECK(saw_interact, "set_keyboard_interactivity(none) — opcode 4");

        /* client: ack_configure + create_pool (with fd!) + 2 buffers + attach + commit */
        bool saw_ack = false, saw_pool = false, saw_attach = false;
        int pool_fd = -1;
        u32 buf_ids[2] = { 0, 0 };
        for (int i = 0; i < 12; i++) {
                n = s_read_msg(msg, sizeof msg, &fd);
                if (n < 8)
                        break;
                if (!s_check(msg, n))
                        break;
                u32 obj = s_get32(msg);
                u32 op = msg_opcode(msg);
                if (obj == S.layer_surface && op == 6) {
                        /* ack_configure is opcode 6 — opcode 5 is get_popup,
                         * opcode 7 is destroy (the 0.1.5 client acked via
                         * the get_popup slot after the table shift) */
                        saw_ack = true;
                        CHECK(s_get32(msg + 8) == 77, "ack_configure(serial=77) [opcode 6]");
                } else if (obj == S.shm && op == 0) {
                        saw_pool = true;
                        S.pool = s_get32(msg + 8);
                        i32 size = (i32)s_get32(msg + 12);
                        CHECK(fd >= 0, "pool fd received via SCM_RIGHTS");
                        /* the engine sizes the pool for PIXEL size (800x600) * 2 buffers */
                        CHECK(size == (i32)(800 * 600 * 4 * 2), "pool size matches pixel buffers");
                        pool_fd = fd;
                } else if (obj == S.pool && op == 0) {
                        u32 id = s_get32(msg + 8);
                        if (!buf_ids[0])
                                buf_ids[0] = id;
                        else
                                buf_ids[1] = id;
                } else if (obj == S.surface && op == 1) {
                        saw_attach = true;
                } else if (obj == S.surface && op == 6) {
                        /* initial commit — last message of the setup phase */
                        break;
                }
        }
        CHECK(saw_ack, "ack_configure seen");
        CHECK(saw_pool, "wl_shm.create_pool seen");
        CHECK(buf_ids[0] && buf_ids[1], "two wl_buffers created");
        CHECK(saw_attach, "first buffer attached");

        /* 7. present a frame: server reads pixels from the pool.
         * The engine framebuffer is PIXEL-sized (800x600 at scale 2). */
        u32 frame[800 * 600];
        for (u32 i = 0; i < 800 * 600; i++)
                frame[i] = 0xff0000ff;    /* opaque blue in ARGB8888 = premul of straight blue */
        /* release the initially-attached buffer so present can cycle slots */
        s_send(buf_ids[0], 0, NULL, 0);
        wl_pump(wl);
        bool ok = wl_present(wl, 0, frame, 800, 600);
        CHECK(ok, "wl_present accepted (hidpi pixel size)");

        /* server: read attach + damage + commit, then verify pixels in the pool */
        void *map = MAP_FAILED;
        if (pool_fd >= 0) {
                map = mmap(NULL, (size_t)800 * 600 * 4 * 2, PROT_READ, MAP_SHARED, pool_fd, 0);
                CHECK(map != MAP_FAILED, "server mmap'ed the client pool");
        }
        bool saw_commit = false;
        for (int i = 0; i < 6; i++) {
                n = s_read_msg(msg, sizeof msg, &fd);
                if (n < 8)
                        break;
                if (!s_check(msg, n))
                        break;
                u32 obj = s_get32(msg);
                u32 op = msg_opcode(msg);
                if (obj == S.surface && op == 6) {
                        saw_commit = true;
                        break;    /* frame commit is the last message of a present */
                }
        }
        CHECK(saw_commit, "frame committed");
        if (map != MAP_FAILED) {
                u32 *px0 = (u32 *)map;
                u32 *px1 = (u32 *)((u8 *)map + (size_t)800 * 600 * 4);
                CHECK(px0[0] == 0xff0000ff, "buffer0 pixel is premultiplied blue");
                /* cycle: release the used slot, present again -> other buffer */
                s_send(buf_ids[1], 0, NULL, 0);
                wl_pump(wl);
                ok = wl_present(wl, 0, frame, 800, 600);
                CHECK(ok && px1[0] == 0xff0000ff, "buffer1 pixel after double-buffer cycle");
        }

        /* 7b. frame callback: wl_present requested one AFTER the commit
         * (wl_surface.frame, opcode 3, new_id payload — the strict validator
         * must accept it; a hallucinated opcode here is a protocol kill on
         * any real compositor). The server replies wl_callback.done(u) and
         * wl_wait_frame() must wake on it — exactly once. */
        {
                n = s_read_msg(msg, sizeof msg, NULL);
                CHECK(n == 12, "frame request is a 12-byte message");
                CHECK(s_get32(msg) == S.surface && msg_opcode(msg) == 3,
                      "frame request: wl_surface opcode 3");
                CHECK(s_check(msg, n), "frame request passes the strict validator");
                u32 cb_id = (n >= 12) ? s_get32(msg + 8) : 0;
                CHECK(cb_id != 0, "frame request carries a new callback id");
                u32 cbdata = 4711;    /* wl_callback.done(callback_data) */
                s_send(cb_id, 0, (const u8 *)&cbdata, 4);
        }
        CHECK(wl_wait_frame(wl, 0.2), "wl_wait_frame wakes on the frame callback");
        CHECK(!wl_wait_frame(wl, 0.02), "callback consumed — single-shot wake");

        /* 7c. dropped frames: server holds BOTH slots -> present is dropped,
         * counted, and does not corrupt the connection (no request sent) */
        ok = wl_present(wl, 0, frame, 800, 600);
        CHECK(ok, "present with both slots held still returns ok");
        CHECK(wl_dropped_frames(wl) == 1, "dropped frame counted");

        /* 7d. refresh rate: the mode event carried 60000 mHz -> 60 Hz */
        CHECK(wl_monitor_refresh(wl, 0) == 60, "wl_monitor_refresh parses mHz -> Hz");

        /* 8. buffer release + pointer events: enter, TWO motions (deltas must
         * accumulate into the TOTAL drag), press + release (a full click that
         * lives entirely between two polls must still register) */
        s_send(buf_ids[0], 0, NULL, 0);   /* release buffer 0 */
        {
                u8 p[24];
                s_put32(p, 5);          /* serial */
                s_put32(p + 4, S.surface);
                s_put32(p + 8, 100 << 8); /* sx = 100.0 fixed */
                s_put32(p + 12, 150 << 8);
                s_send(A.pointer_id, 0, p, 16);   /* enter */
                u8 m[16];
                s_put32(m, 999);        /* time */
                s_put32(m + 4, 120 << 8);
                s_put32(m + 8, 180 << 8);
                s_send(A.pointer_id, 2, m, 12);   /* motion (100,150)->(120,180) */
                u8 m2[16];
                s_put32(m2, 1001);      /* time */
                s_put32(m2 + 4, 140 << 8);
                s_put32(m2 + 8, 200 << 8);
                s_send(A.pointer_id, 2, m2, 12);  /* motion (120,180)->(140,200) */
                u8 b[20];
                s_put32(b, 7);          /* serial */
                s_put32(b + 4, 1000);   /* time */
                s_put32(b + 8, 272);    /* left button */
                s_put32(b + 12, 1);     /* pressed */
                s_send(A.pointer_id, 3, b, 16);   /* button press */
                u8 b2[20];
                s_put32(b2, 8);         /* serial */
                s_put32(b2 + 4, 1002);  /* time */
                s_put32(b2 + 8, 272);   /* left button */
                s_put32(b2 + 12, 0);    /* released */
                s_send(A.pointer_id, 3, b2, 16);  /* button release */
        }
        wl_pump(wl);
        RPointerState pst;
        CHECK(wl_pointer(wl, &pst), "pointer state valid");
        CHECK(pst.x == 140.0 && pst.y == 200.0, "pointer at (140,200)");
        CHECK(pst.dx == 40.0 && pst.dy == 50.0,
              "drag deltas accumulate across motion events");
        CHECK(pst.click, "full click latched (press+release between reads)");
        CHECK(pst.click_x == 140.0 && pst.click_y == 200.0,
              "click position latched at the press");
        CHECK(pst.clicks == 1, "click count monotonic");
        CHECK(!pst.pressed, "button released");
        RPointerState pst2;
        CHECK(wl_pointer(wl, &pst2), "pointer state still valid");
        CHECK(pst2.dx == 0.0 && pst2.dy == 0.0 && !pst2.click,
              "consume-on-read: no stale deltas or click repeats");
        CHECK(pst2.clicks == 1, "total click count survives reads");

        /* 9. closed event */
        s_send(S.layer_surface, 1, NULL, 0);
        wl_pump(wl);
        CHECK(wl_should_close(wl), "layer surface closed detected");

        /* 10. wl_display.error: the client must flag the connection fatal
         * (and not crash or mis-parse the string payload) */
        {
                const char *emsg = "test: synthetic error";
                u32 sl = (u32)strlen(emsg) + 1;
                u32 pad = (4 - (sl & 3)) & 3;
                u8 p[64];
                memset(p, 0, sizeof p);
                s_put32(p, S.surface);     /* offending object */
                s_put32(p + 4, 1);         /* code */
                s_put32(p + 8, sl);
                memcpy(p + 12, emsg, sl);
                s_send(OBJ_DISPLAY, 0, p, 12 + sl + pad);
        }
        wl_pump(wl);
        CHECK(wl_fatal(wl), "wl_display.error flagged fatal");

        /* the strict validator must have seen ZERO violations from the client
         * across the whole session (handshake, binds, layer-surface setup,
         * pool/buffers, present, pointer) — any protocol slip the lenient
         * 0.1.2 fake would have waved through now fails the test here. */
        CHECK(!srv_error_posted, "strict validator: zero protocol violations");
        if (srv_error_posted)
                printf("    strict validator says: %s\n", srv_err);

        wl_disconnect(wl);
        close(cli_fd);
        close(srv_fd);
        unlink(sa.sun_path);
        rmdir(runtime_dir);

        if (failures == 0) {
                printf("wayland: OK\n");
                return 0;
        }
        printf("wayland: %d failure(s)\n", failures);
        return 1;
}
