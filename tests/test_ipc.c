/* ============================================================================
 * Reverie Engine — tests/test_ipc.c
 * Protocol-level tests for the runtime control socket: handshake versioning,
 * framing limits, malformed input, disconnects, stale-socket reclaim, stale
 * "in-use" detection, and the per-user permission bits. The DAEMON-side
 * command semantics (theme switching, fades, playlists) are exercised
 * end-to-end under Xvfb by scripts/e2e.sh through revtool ipc.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "../src/ipc.h"

static int failures;

#define CHECK(cond, msg)                                                \
        do {                                                            \
                if (cond) {                                             \
                        printf("  ok: %s\n", msg);                      \
                } else {                                                \
                        printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
                        failures++;                                     \
                }                                                       \
        } while (0)

/* ---- test handler: echoes commands, implements THEME validation ---------- */

static char seen[16][64];
static int seen_n;
static int theme_calls;          /* THEME commands seen          */
static int theme_fails;          /* THEME paths containing "bad" */

static void test_handler(void *ctx, const IpcMsg *msg, IpcReply *rep)
{
        (void)ctx;
        if (seen_n < 16) {
                snprintf(seen[seen_n], sizeof seen[0], "%s", msg->argv[0]);
                seen_n++;
        }
        if (strcmp(msg->argv[0], "PING") == 0) {
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "PONG");
        } else if (strcmp(msg->argv[0], "THEME") == 0) {
                theme_calls++;
                if (msg->argc >= 2 && strstr(msg->argv[1], "bad")) {
                        rep->ok = false;
                        snprintf(rep->text, sizeof rep->text, "cannot load");
                        theme_fails++;
                } else {
                        rep->ok = true;
                        snprintf(rep->text, sizeof rep->text, "switched");
                }
        } else if (strcmp(msg->argv[0], "ARGS") == 0) {
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "argc=%d", msg->argc);
        } else if (strcmp(msg->argv[0], "ECHO2") == 0) {
                rep->ok = true;
                snprintf(rep->text, sizeof rep->text, "%s|%s",
                         msg->argc > 1 ? msg->argv[1] : "",
                         msg->argc > 2 ? msg->argv[2] : "");
        } else {
                rep->ok = false;
                snprintf(rep->text, sizeof rep->text, "unknown-command");
        }
}

/* ---- raw client helpers (deliberately NOT ipc_request) -------------------- */

static int raw_connect(const char *path)
{
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
                return -1;
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        if (strlen(path) >= sizeof sa.sun_path) {
                close(fd);
                return -1;
        }
        snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
                close(fd);
                return -1;
        }
        return fd;
}

/* sends one line, reads one reply line (bounded wait) */
static bool raw_roundtrip(int fd, const char *line, char *reply, size_t cap)
{
        char buf[1024];
        snprintf(buf, sizeof buf, "%s\n", line);
        if (write(fd, buf, strlen(buf)) < 0)
                return false;
        size_t n = 0;
        for (;;) {
                struct pollfd pf = { fd, POLLIN, 0 };
                if (poll(&pf, 1, 2000) <= 0)
                        return false;
                char ch;
                ssize_t k = read(fd, &ch, 1);
                if (k <= 0)
                        return false;
                if (ch == '\n') {
                        reply[n] = 0;
                        return true;
                }
                if (ch != '\r' && n + 1 < cap)
                        reply[n++] = ch;
        }
}

/* reads until EOF (bounded): a dropped client may still owe us an ERR
 * reply line queued before the close */
static bool client_got_dropped(int fd)
{
        char buf[256];
        int total = 0;
        for (int spin = 0; spin < 40; spin++) {
                struct pollfd pf = { fd, POLLIN, 0 };
                if (poll(&pf, 1, 200) <= 0)
                        return false;    /* still connected, nothing came */
                ssize_t k = read(fd, buf, sizeof buf);
                if (k == 0)
                        return true;     /* EOF: dropped */
                if (k < 0)
                        return false;
                total += (int)k;
                if (total > 4096)
                        return false;    /* way too much: not a drop */
        }
        return false;
}

/* ---- pump thread: the daemon pumps from its frame loop; the test needs a
 * thread so ipc_request (blocking client) and the server can coexist ----- */
static IpcServer *g_srv;
static volatile int g_pump_run;

static void *pump_thread(void *arg)
{
        (void)arg;
        while (g_pump_run) {
                ipc_serve_poll(g_srv, test_handler, NULL);
                usleep(2000);
        }
        return NULL;
}

int main(void)
{
        setvbuf(stdout, NULL, _IOLBF, 0);
        alarm(60);

        char sock[128];
        snprintf(sock, sizeof sock, "/tmp/revipc.%d.sock", (int)getpid());
        unlink(sock);
        unsetenv("REV_IPC_SOCK");
        unsetenv("XDG_RUNTIME_DIR");    /* force explicit path behavior */

        char err[256] = { 0 };

        /* 1. server start + socket permissions + the pump thread */
        IpcServer *s = ipc_serve_start(sock, err, sizeof err);
        CHECK(s != NULL, err[0] ? err : "server started");
        if (!s)
                return 1;
        g_srv = s;
        g_pump_run = 1;
        pthread_t th;
        pthread_create(&th, NULL, pump_thread, NULL);
        CHECK(!strcmp(ipc_serve_path(s), sock), "bound path reported");
        {
                struct stat st;
                CHECK(stat(sock, &st) == 0 && (st.st_mode & 0777) == 0600,
                      "socket is 0600 (owner-only — per-user)");
        }

        /* 2. proper client roundtrips via ipc_request */
        char rep[IPC_REPLY_MAX + 32];
        CHECK(ipc_request(sock, "PING", rep, sizeof rep, 2000) &&
              !strcmp(rep, "OK PONG"), "PING -> OK PONG via ipc_request");
        CHECK(!ipc_request(sock, "NOPE", rep, sizeof rep, 2000) &&
              !strcmp(rep, "ERR unknown-command"), "unknown command -> ERR");
        CHECK(ipc_request(sock, "THEME themes/good", rep, sizeof rep, 2000),
              "THEME (good) -> OK");

        /* 3. raw protocol: handshake rules */
        {
                int fd = raw_connect(sock);
                CHECK(fd >= 0, "raw client connected");
                char r[256];
                CHECK(raw_roundtrip(fd, "PING", r, sizeof r) &&
                      !strcmp(r, "ERR need-hello"),
                      "command before HELLO -> ERR need-hello");
                CHECK(raw_roundtrip(fd, "HELLO 1", r, sizeof r) &&
                      !strcmp(r, "OK 1"), "HELLO 1 -> OK 1");
                CHECK(raw_roundtrip(fd, "PING", r, sizeof r) &&
                      !strcmp(r, "OK PONG"), "PING after HELLO -> OK PONG");
                CHECK(raw_roundtrip(fd, "HELLO 1", r, sizeof r) &&
                      !strcmp(r, "ERR already-helloed"),
                      "double HELLO -> ERR already-helloed");
                close(fd);
        }
        {
                int fd = raw_connect(sock);
                char r[256];
                CHECK(raw_roundtrip(fd, "HELLO 99", r, sizeof r) &&
                      !strcmp(r, "ERR bad-version"),
                      "HELLO with future version -> ERR bad-version");
                /* server must have dropped the connection */
                struct pollfd pf = { fd, POLLIN, 0 };
                char ch;
                int dropped = poll(&pf, 1, 300) > 0 &&
                              read(fd, &ch, 1) == 0;
                CHECK(dropped, "protocol-mismatched client was disconnected");
                close(fd);
        }

        /* 4. argument handling */
        {
                CHECK(ipc_request(sock, "ARGS a b c", rep, sizeof rep, 2000) &&
                      !strcmp(rep, "OK argc=4"), "argument count passed through");
                CHECK(ipc_request(sock, "ECHO2 hello world", rep, sizeof rep, 2000) &&
                      !strcmp(rep, "OK hello|world"), "argument contents intact");
        }

        /* 5. malformed input: binary garbage drops the client, server lives */
        {
                int fd = raw_connect(sock);
                char r[256];
                CHECK(raw_roundtrip(fd, "HELLO 1", r, sizeof r), "garbage leg handshake");
                const char junk[] = { 'P', 0x01, 0x02, (char)0xFF, '\n' };
                CHECK(write(fd, junk, sizeof junk) > 0, "binary garbage sent");
                usleep(100000);    /* pump thread processes + drops */
                CHECK(client_got_dropped(fd), "binary garbage dropped the client");
                close(fd);
                CHECK(ipc_request(sock, "PING", rep, sizeof rep, 2000),
                      "server still healthy after garbage");
        }

        /* 6. oversized line: ERR line-too-long, connection stays usable
         * (the overflow state resets at the newline — a client that made
         * one mistake recovers instead of being dropped) */
        {
                int fd = raw_connect(sock);
                char r[256];
                CHECK(raw_roundtrip(fd, "HELLO 1", r, sizeof r), "oversize leg handshake");
                char big[5000];
                memset(big, 'x', sizeof big - 1);
                big[sizeof big - 1] = 0;
                CHECK(write(fd, big, strlen(big)) > 0 &&
                      write(fd, "\n", 1) == 1, "oversized line sent");
                CHECK(raw_roundtrip(fd, "PING", r, sizeof r) &&
                      !strcmp(r, "ERR line-too-long"),
                      "oversized line -> ERR line-too-long");
                CHECK(raw_roundtrip(fd, "PING", r, sizeof r) &&
                      !strcmp(r, "OK PONG"),
                      "connection resynced after the oversized line");
                close(fd);
                CHECK(ipc_request(sock, "PING", rep, sizeof rep, 2000),
                      "server still healthy after oversize");
        }

        /* 7. partial line then disconnect: nothing runs, nothing leaks */
        {
                int fd = raw_connect(sock);
                char r[256];
                CHECK(raw_roundtrip(fd, "HELLO 1", r, sizeof r), "partial leg handshake");
                CHECK(write(fd, "PIN", 3) == 3, "partial command sent");
                close(fd);    /* mid-line disconnect */
                int before = seen_n;
                usleep(100000);
                CHECK(seen_n == before, "partial line never ran");
                CHECK(ipc_request(sock, "PING", rep, sizeof rep, 2000),
                      "server healthy after mid-line disconnect");
        }

        /* 8. THEME failure semantics via the handler */
        {
                CHECK(!ipc_request(sock, "THEME /bad/path", rep, sizeof rep, 2000) &&
                      !strcmp(rep, "ERR cannot load"), "THEME (bad) -> ERR with reason");
                CHECK(theme_calls == 2 && theme_fails == 1,
                      "exactly the two THEME commands ran (no duplicates)");
        }

        /* 9. stale-socket reclaim: dead file -> bind succeeds */
        {
                g_pump_run = 0;
                pthread_join(th, NULL);
                ipc_serve_stop(s);    /* leaves NO file (clean stop unlinks) */
                /* fabricate a stale socket file: a bound-but-dead socket */
                int fake = socket(AF_UNIX, SOCK_STREAM, 0);
                struct sockaddr_un sa;
                memset(&sa, 0, sizeof sa);
                sa.sun_family = AF_UNIX;
                {
                        size_t plen = strlen(sock);
                        if (plen >= sizeof sa.sun_path) {
                                close(fake);
                                ipc_serve_stop(s);
                                CHECK(false, "test sock path unexpectedly long");
                                return 1;
                        }
                        memcpy(sa.sun_path, sock, plen);
                        sa.sun_path[plen] = 0;
                }
                bind(fake, (struct sockaddr *)&sa, sizeof sa);
                listen(fake, 1);
                close(fake);    /* file remains, nothing listens */
                IpcServer *s2 = ipc_serve_start(sock, err, sizeof err);
                CHECK(s2 != NULL, "stale socket file reclaimed (crashed daemon)");
                if (s2)
                        ipc_serve_stop(s2);
        }

        /* 10. live server is never clobbered: "in-use" */
        {
                IpcServer *s3 = ipc_serve_start(sock, err, sizeof err);
                CHECK(s3 != NULL, "second generation started");
                /* this PING needs a pump too */
                g_srv = s3;
                g_pump_run = 1;
                pthread_create(&th, NULL, pump_thread, NULL);
                IpcServer *s4 = ipc_serve_start(sock, err, sizeof err);
                CHECK(s4 == NULL && !strcmp(err, "in-use"),
                      "live socket refused with in-use (never clobbered)");
                CHECK(ipc_request(sock, "PING", rep, sizeof rep, 2000),
                      "original server still answering after the refusal");
                g_pump_run = 0;
                pthread_join(th, NULL);
                if (s3)
                        ipc_serve_stop(s3);
        }

        /* 11. default path resolution */
        {
                char p[256];
                char e[128];
                setenv("XDG_RUNTIME_DIR", "/tmp", 1);
                CHECK(ipc_default_path(p, sizeof p, e, sizeof e) &&
                      !strcmp(p, "/tmp/reverie.sock"),
                      "default path honors XDG_RUNTIME_DIR");
                setenv("REV_IPC_SOCK", "/tmp/custom.sock", 1);
                CHECK(ipc_default_path(p, sizeof p, e, sizeof e) &&
                      !strcmp(p, "/tmp/custom.sock"),
                      "REV_IPC_SOCK overrides the location");
                unsetenv("REV_IPC_SOCK");
                unsetenv("XDG_RUNTIME_DIR");
                unsetenv("HOME");
                CHECK(!ipc_default_path(p, sizeof p, e, sizeof e),
                      "no HOME/XDG -> clear failure, not a bad guess");
        }

        unlink(sock);
        if (failures == 0) {
                printf("ipc: OK\n");
                return 0;
        }
        printf("ipc: %d failure(s)\n", failures);
        return 1;
}
