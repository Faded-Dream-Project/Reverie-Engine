/* ============================================================================
 * Reverie Engine — tests/test_wayland.c
 * E2E test of the raw Wayland wire client against a hand-written fake
 * compositor over a unix socket: handshake, registry, binds (byte-exact),
 * layer-surface configure flow, wl_shm pool with a real memfd (server
 * mmaps it and checks PIXELS), buffer attach/commit, and pointer events.
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
static volatile bool saw_anchor = false, saw_size = false;

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

/* ---- the fake compositor's object ids ---------------------------------------
 * The client picks its own ids starting at 2 (registry) then 3+ for binds.
 * The SERVER just needs to remember which id maps to what. */
#define OBJ_DISPLAY 1
#define OBJ_REGISTRY 2

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
                u32 pointer_id = s_get32(msg + 8);
                n = s_read_msg(msg, sizeof msg, NULL);
                u32 sync2 = s_get32(msg + 8);
                CHECK(msg_opcode(msg) == 0, "second sync");
                s_send(sync2, 0, NULL, 0);
                A.pointer_id = pointer_id;
                A.sync2 = sync2;
                return NULL;
}

static void *phase_b(void *arg)
{
        (void)arg;
        n = s_read_msg(msg, sizeof msg, NULL);
        CHECK(s_get32(msg) == S.compositor && msg_opcode(msg) == 0,
              "compositor.create_surface");
        S.surface = s_get32(msg + 8);

        n = s_read_msg(msg, sizeof msg, NULL);
        CHECK(s_get32(msg) == S.layer_shell && msg_opcode(msg) == 1,
              "layer_shell.get_layer_surface");
        S.layer_surface = s_get32(msg + 8);
        u32 surf_arg = s_get32(msg + 12);
        u32 output_arg = s_get32(msg + 16);
        u32 layer = s_get32(msg + 20);
        CHECK(surf_arg == S.surface, "get_layer_surface(surface) is our surface");
        CHECK(output_arg == S.output, "get_layer_surface(output) is our output");
        CHECK(layer == 1, "layer == BACKGROUND(1)");

        /* drain set_anchor/zone/interactivity/size + commit(null) */
        for (int i = 0; i < 8; i++) {
                n = s_read_msg(msg, sizeof msg, NULL);
                if (n < 8)
                break;
                u32 op = msg_opcode(msg);
                u32 obj = s_get32(msg);
                if (obj == S.layer_surface && op == 1 && n >= 12 && s_get32(msg + 8) == 15)
                saw_anchor = true;
                if (obj == S.layer_surface && op == 0 && n >= 16)
                saw_size = true;
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
        CHECK(saw_anchor, "set_anchor(all edges)");
        CHECK(saw_size, "set_size");

        /* client: ack_configure + create_pool (with fd!) + 2 buffers + attach + commit */
        bool saw_ack = false, saw_pool = false, saw_attach = false;
        int pool_fd = -1;
        u32 buf_ids[2] = { 0, 0 };
        for (int i = 0; i < 12; i++) {
                n = s_read_msg(msg, sizeof msg, &fd);
                if (n < 8)
                        break;
                u32 obj = s_get32(msg);
                u32 op = msg_opcode(msg);
                if (obj == S.layer_surface && op == 6) {
                        saw_ack = true;
                        CHECK(s_get32(msg + 8) == 77, "ack_configure(serial=77)");
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

        /* 8. buffer release + pointer events */
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
                s_send(A.pointer_id, 2, m, 12);   /* motion */
                u8 b[20];
                s_put32(b, 7);          /* serial */
                s_put32(b + 4, 1000);   /* time */
                s_put32(b + 8, 272);    /* left button */
                s_put32(b + 12, 1);     /* pressed */
                s_send(A.pointer_id, 3, b, 16);   /* button */
        }
        wl_pump(wl);
        f64 px, py, dx, dy;
        bool pressed;
        CHECK(wl_pointer(wl, &px, &py, &dx, &dy, &pressed), "pointer state valid");
        CHECK(px == 120.0 && py == 180.0, "pointer at (120,180)");
        CHECK(pressed, "pointer pressed");

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
