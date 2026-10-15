/* ============================================================================
 * Reverie Engine — ipc.c
 * Runtime control socket implementation. Single-threaded by design: the
 * daemon pumps it from its frame loop, so no locks, no threads, and every
 * poll does BOUNDED work (a read cap per client, a command cap per poll).
 *
 * Security posture:
 *   - the socket lives in a per-user location and is chmod'd 0600;
 *   - SO_PEERCRED rejects any connection whose uid is not ours (belt and
 *     braces for a mis-placed socket file);
 *   - the protocol is text commands only — no shell, no eval, no path is
 *     touched except by the caller's own handler (the daemon's THEME
 *     command goes through the same theme loader the CLI uses).
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#define _GNU_SOURCE    /* struct ucred / SO_PEERCRED */

#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"
#include "util.h"

#define IPC_MAX_CLIENTS 8
#define IPC_READ_CHUNK  512     /* bytes max per client per poll     */
#define IPC_CMDS_PER_POLL 8     /* complete lines max per client     */

typedef struct {
        int fd;
        bool helloed;
        size_t n;               /* bytes buffered for the current line */
        bool overflow;          /* line exceeded the cap: discard until \n */
        char line[IPC_MAX_LINE + 1];
} IpcClient;

struct IpcServer {
        int lfd;
        char path[384];
        IpcClient cli[IPC_MAX_CLIENTS];
};

/* ---- small helpers --------------------------------------------------------- */

static int set_nonblock(int fd, bool on)
{
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl < 0)
                return -1;
        return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

static bool send_line(int fd, const char *s)
{
        size_t len = strlen(s);
        const char *p = s;
        int tries = 0;
        while (len > 0) {
                /* MSG_NOSIGNAL: a client that hung up mid-reply must never
                 * SIGPIPE the daemon (write() would) */
#ifdef MSG_NOSIGNAL
                ssize_t k = send(fd, p, len, MSG_NOSIGNAL);
#else
                ssize_t k = write(fd, p, len);
#endif
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                if (++tries > 10)
                                        return false;
                                struct pollfd pf = { fd, POLLOUT, 0 };
                                poll(&pf, 1, 10);
                                continue;
                        }
                        return false;
                }
                p += k;
                len -= (size_t)k;
        }
        return true;
}

static void reply_ok(IpcClient *c, const char *text)
{
        char buf[IPC_REPLY_MAX + 16];
        snprintf(buf, sizeof buf, "OK %s\n", text && *text ? text : "");
        send_line(c->fd, buf);
}

static void reply_err(IpcClient *c, const char *text)
{
        char buf[IPC_REPLY_MAX + 16];
        snprintf(buf, sizeof buf, "ERR %s\n", text && *text ? text : "");
        send_line(c->fd, buf);
}

static void client_close(IpcServer *s, int i)
{
        close(s->cli[i].fd);
        s->cli[i].fd = -1;
}

/* Runs one complete command line through the handler (or the core's own
 * HELLO handling). `line` is NUL-terminated, without the newline. */
static void run_line(IpcServer *s, IpcClient *c, char *line, IpcCmdFn fn, void *ctx)
{
        /* split into arguments (whitespace); in-place, bounded */
        char *argv[IPC_MAX_ARGS];
        int argc = 0;
        char *p = line;
        while (*p && argc < IPC_MAX_ARGS) {
                while (*p == ' ' || *p == '\t')
                        p++;
                if (!*p)
                        break;
                argv[argc++] = p;
                while (*p && *p != ' ' && *p != '\t')
                        p++;
                if (*p)
                        *p++ = 0;
        }

        if (argc == 0) {
                reply_err(c, "empty-command");
                return;
        }

        /* the core owns the versioned handshake */
        if (strcmp(argv[0], "HELLO") == 0) {
                if (argc != 2) {
                        reply_err(c, "usage: HELLO <version>");
                        return;
                }
                if (c->helloed) {
                        reply_err(c, "already-helloed");
                        return;
                }
                int v = atoi(argv[1]);
                if (v < 1 || v > IPC_VERSION) {
                        reply_err(c, "bad-version");
                        /* protocol mismatch: no point continuing */
                        client_close(s, (int)(c - s->cli));
                        return;
                }
                c->helloed = true;
                char rep[32];
                snprintf(rep, sizeof rep, "%d", IPC_VERSION);
                reply_ok(c, rep);
                return;
        }

        if (!c->helloed) {
                reply_err(c, "need-hello");
                return;
        }

        if (!fn) {
                reply_err(c, "no-handler");
                return;
        }
        IpcMsg msg;
        msg.argc = argc;
        for (int i = 0; i < argc; i++)
                msg.argv[i] = argv[i];
        IpcReply rep;
        memset(&rep, 0, sizeof rep);
        rep.ok = false;
        snprintf(rep.text, sizeof rep.text, "no-reply");
        fn(ctx, &msg, &rep);
        if (rep.ok)
                reply_ok(c, rep.text);
        else
                reply_err(c, rep.text);
}

/* Feeds buffered bytes to the line splitter; runs complete lines. */
static void client_feed(IpcServer *s, IpcClient *c, const char *buf, size_t len,
                        IpcCmdFn fn, void *ctx)
{
        int ran = 0;
        for (size_t i = 0; i < len; i++) {
                if (buf[i] == '\n') {
                        if (c->overflow) {
                                c->overflow = false;
                                c->n = 0;
                                reply_err(c, "line-too-long");
                                continue;
                        }
                        c->line[c->n] = 0;
                        c->n = 0;
                        if (c->fd < 0)
                                return;    /* HELLO mismatch closed us */
                        run_line(s, c, c->line, fn, ctx);
                        ran++;
                        if (c->fd < 0)
                                return;    /* handler closed us */
                        if (ran >= IPC_CMDS_PER_POLL)
                                return;    /* bounded work; rest waits */
                        continue;
                }
                if (buf[i] == '\r')
                        continue;          /* tolerate CRLF */
                if (c->overflow)
                        continue;
                if (c->n >= IPC_MAX_LINE) {
                        c->overflow = true;
                        c->n = 0;
                        continue;
                }
                c->line[c->n++] = buf[i];
        }
}

/* ---- server ---------------------------------------------------------------- */

static bool path_is_live(const char *path)
{
        size_t plen = strlen(path);
        if (plen >= 107)            /* sun_path cap; oversize = unserveable */
                return false;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
                return false;
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        memcpy(sa.sun_path, path, plen);
        sa.sun_path[plen] = 0;
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
                close(fd);
                return false;    /* nothing there (or stale socket file) */
        }
        /* something answered: is it an IPC server? PING it. */
        set_nonblock(fd, true);
        send_line(fd, "HELLO 1\nPING\n");
        struct pollfd pf = { fd, POLLIN, 0 };
        char buf[128];
        bool live = false;
        if (poll(&pf, 1, 250) > 0) {
                ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
                if (n > 0) {
                        buf[n] = 0;
                        live = strstr(buf, "OK 1") == buf || strstr(buf, "PONG") != NULL;
                }
        }
        close(fd);
        return live;
}

IpcServer *ipc_serve_start(const char *path, char *err, size_t errcap)
{
        char pbuf[384];
        if (!path || !*path) {
                if (!ipc_default_path(pbuf, sizeof pbuf, err, errcap))
                        return NULL;
                path = pbuf;
        }

        IpcServer *s = (IpcServer *)calloc(1, sizeof *s);
        if (!s) {
                snprintf(err, errcap, "out of memory");
                return NULL;
        }
        snprintf(s->path, sizeof s->path, "%s", path);
        for (int i = 0; i < IPC_MAX_CLIENTS; i++)
                s->cli[i].fd = -1;

        /* never clobber a live daemon's socket */
        if (path_is_live(s->path)) {
                snprintf(err, errcap, "in-use");
                free(s);
                return NULL;
        }
        unlink(s->path);    /* stale file from a crashed daemon */

        s->lfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s->lfd < 0) {
                snprintf(err, errcap, "socket: %s", strerror(errno));
                free(s);
                return NULL;
        }
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        if (strlen(s->path) >= sizeof sa.sun_path) {
                snprintf(err, errcap, "socket path too long");
                close(s->lfd);
                free(s);
                return NULL;
        }
        {
                size_t plen = strlen(s->path);
                memcpy(sa.sun_path, s->path, plen);
                sa.sun_path[plen] = 0;
        }
        if (bind(s->lfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
                snprintf(err, errcap, "bind %s: %s", s->path, strerror(errno));
                close(s->lfd);
                free(s);
                return NULL;
        }
        /* owner-only: no other user may command this daemon */
        chmod(s->path, 0600);
        if (listen(s->lfd, 4) < 0) {
                snprintf(err, errcap, "listen: %s", strerror(errno));
                unlink(s->path);
                close(s->lfd);
                free(s);
                return NULL;
        }
        set_nonblock(s->lfd, true);
        return s;
}

void ipc_serve_stop(IpcServer *s)
{
        if (!s)
                return;
        for (int i = 0; i < IPC_MAX_CLIENTS; i++)
                if (s->cli[i].fd >= 0)
                        close(s->cli[i].fd);
        close(s->lfd);
        unlink(s->path);
        free(s);
}

static void accept_clients(IpcServer *s)
{
        for (;;) {
                int fd = accept(s->lfd, NULL, NULL);
                if (fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK ||
                            errno == EINTR)
                                return;
                        return;    /* transient: try again next poll */
                }
#ifdef SO_PEERCRED
                {
                        struct ucred cr;
                        socklen_t cl = sizeof cr;
                        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 &&
                            cr.uid != geteuid()) {
                                LOGW("ipc: connection from uid %ld refused "
                                     "(socket is per-user)", (long)cr.uid);
                                close(fd);
                                continue;
                        }
                }
#endif
                int slot = -1;
                for (int i = 0; i < IPC_MAX_CLIENTS; i++)
                        if (s->cli[i].fd < 0) {
                                slot = i;
                                break;
                        }
                if (slot < 0) {
                        /* full: politely bounce instead of blocking */
                        send_line(fd, "ERR busy\n");
                        close(fd);
                        continue;
                }
                set_nonblock(fd, true);
                memset(&s->cli[slot], 0, sizeof s->cli[slot]);
                s->cli[slot].fd = fd;
        }
}

void ipc_serve_poll(IpcServer *s, IpcCmdFn fn, void *ctx)
{
        if (!s || s->lfd < 0)
                return;
        accept_clients(s);

        struct pollfd pf[IPC_MAX_CLIENTS];
        int idx[IPC_MAX_CLIENTS];
        int n = 0;
        for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
                if (s->cli[i].fd < 0)
                        continue;
                pf[n].fd = s->cli[i].fd;
                pf[n].events = POLLIN;
                pf[n].revents = 0;
                idx[n] = i;
                n++;
        }
        if (n == 0)
                return;
        if (poll(pf, (nfds_t)n, 0) <= 0)
                return;

        char buf[IPC_READ_CHUNK];
        for (int k = 0; k < n; k++) {
                if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR)))
                        continue;
                IpcClient *c = &s->cli[idx[k]];
                ssize_t got = recv(c->fd, buf, sizeof buf, 0);
                if (got < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK ||
                            errno == EINTR)
                                continue;
                        client_close(s, idx[k]);
                        continue;
                }
                if (got == 0) {
                        client_close(s, idx[k]);
                        continue;    /* client disconnected */
                }
                /* binary garbage safety: reject non-printable bytes that
                 * are not CR/LF/tab outright (drop the client) */
                bool bad = false;
                for (ssize_t i = 0; i < got; i++) {
                        if ((unsigned char)buf[i] < 0x20 && buf[i] != '\t' &&
                            buf[i] != '\n' && buf[i] != '\r') {
                                bad = true;
                                break;
                        }
                }
                if (bad) {
                        reply_err(c, "binary-input");
                        client_close(s, idx[k]);
                        continue;
                }
                client_feed(s, c, buf, (size_t)got, fn, ctx);
        }
}

const char *ipc_serve_path(const IpcServer *s)
{
        return s ? s->path : NULL;
}

/* ---- client ---------------------------------------------------------------- */

static int connect_timeout(const char *path, int timeout_ms)
{
        size_t plen = strlen(path);
        if (plen >= 107)            /* sun_path cap */
                return -1;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
                return -1;
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        memcpy(sa.sun_path, path, plen);
        sa.sun_path[plen] = 0;
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
                close(fd);
                return -1;
        }
        /* read_line paces itself with poll timeouts — a blocking recv
         * could hang the client forever on a stalled server */
        set_nonblock(fd, true);
        (void)timeout_ms;    /* unix connect is effectively immediate */
        return fd;
}

/* reads one '\n'-terminated line with a timeout; returns length or -1 */
static int read_line(int fd, char *out, size_t cap, int timeout_ms)
{
        size_t n = 0;
        f64 deadline = rev_monotonic() + (f64)timeout_ms / 1000.0;
        for (;;) {
                char ch;
                ssize_t k = recv(fd, &ch, 1, 0);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                f64 rem = deadline - rev_monotonic();
                                if (rem <= 0)
                                        return -1;
                                struct pollfd pf = { fd, POLLIN, 0 };
                                int ms = (int)(rem * 1000.0);
                                if (ms < 1)
                                        ms = 1;
                                if (poll(&pf, 1, ms) <= 0)
                                        return -1;
                                continue;
                        }
                        return -1;
                }
                if (k == 0)
                        return -1;
                if (ch == '\n') {
                        out[n] = 0;
                        return (int)n;
                }
                if (ch != '\r') {
                        if (n + 1 >= cap)
                                return -1;    /* reply too long */
                        out[n++] = ch;
                }
                if (rev_monotonic() > deadline)
                        return -1;
        }
}

bool ipc_request(const char *path, const char *cmdline,
                 char *reply, size_t cap, int timeout_ms)
{
        if (reply && cap)
                reply[0] = 0;
        char pbuf[384];
        if (!path || !*path) {
                char err[128];
                if (!ipc_default_path(pbuf, sizeof pbuf, err, sizeof err)) {
                        if (reply && cap)
                                snprintf(reply, cap, "%s", err);
                        return false;
                }
                path = pbuf;
        }
        int fd = connect_timeout(path, timeout_ms);
        if (fd < 0) {
                if (reply && cap)
                        snprintf(reply, cap, "cannot connect to %s", path);
                return false;
        }
        bool ok = false;
        char line[IPC_REPLY_MAX + 32];

        if (!send_line(fd, "HELLO 1\n"))
                goto out;
        if (read_line(fd, line, sizeof line, timeout_ms) < 0)
                goto out;
        if (strncmp(line, "OK", 2) != 0) {
                if (reply && cap)
                        snprintf(reply, cap, "%s", line);
                goto out;
        }
        if (strchr(cmdline, '\n')) {
                if (reply && cap)
                        snprintf(reply, cap, "command must not contain a newline");
                goto out;
        }
        {
                char sendbuf[IPC_MAX_LINE + 2];
                snprintf(sendbuf, sizeof sendbuf, "%s\n", cmdline);
                if (!send_line(fd, sendbuf))
                        goto out;
        }
        if (read_line(fd, line, sizeof line, timeout_ms) < 0) {
                if (reply && cap)
                        snprintf(reply, cap, "no reply (timeout)");
                goto out;
        }
        if (reply && cap)
                snprintf(reply, cap, "%s", line);
        ok = strncmp(line, "OK", 2) == 0;
out:
        close(fd);
        return ok;
}

/* ---- default path ----------------------------------------------------------- */

bool ipc_default_path(char *buf, size_t cap, char *err, size_t errcap)
{
        const char *e = getenv("REV_IPC_SOCK");
        if (e && *e) {
                snprintf(buf, cap, "%s", e);
                return true;
        }
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg && *xdg) {
                snprintf(buf, cap, "%s/reverie.sock", xdg);
                return true;
        }
        const char *home = getenv("HOME");
        if (home && *home) {
                char dir[320];
                snprintf(dir, sizeof dir, "%s/.cache/reverie", home);
                mkdir(dir, 0700);    /* best effort; bind fails loudly below */
                chmod(dir, 0700);
                snprintf(buf, cap, "%s/reverie.sock", dir);
                return true;
        }
        snprintf(err, errcap, "no per-user runtime location "
                              "(XDG_RUNTIME_DIR and HOME are unset)");
        return false;
}
