/* ============================================================================
 * Reverie Engine — ipc.h
 * Runtime control socket: a running daemon can switch themes, manage
 * playlists, and shut down without restarting (revtool ipc is the client).
 *
 * Protocol v1 — text lines over a unix socket:
 *   client connects, then MUST send:   HELLO 1
 *   server replies:                    OK 1
 *   then one command per line:         PING / STATUS / THEME <path> [fade_ms]
 *                                       / PLAYLIST <p...> / NEXT [fade_ms]
 *                                       / PREV [fade_ms] / ROTATE <sec>
 *                                       / FADE <ms> / QUIT
 *   every reply is one line:           OK <text>   |   ERR <text>
 *
 * Bounded by design: lines <= 4096 bytes, <= 16 arguments, one socket per
 * user (0600 perms + SO_PEERCRED uid check), no shell, no file access
 * beyond what the theme loader itself does.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_IPC_H
#define REV_IPC_H

#include <stdbool.h>
#include <stddef.h>

#define IPC_VERSION     1
#define IPC_MAX_LINE    4096
#define IPC_MAX_ARGS    16
#define IPC_REPLY_MAX   384

typedef struct IpcServer IpcServer;

/* One parsed command line (argv[0] is the command word). Storage is owned
 * by the server and valid only during the handler call. */
typedef struct {
        int argc;
        const char *argv[IPC_MAX_ARGS];
} IpcMsg;

/* Handler reply: ok=true sends "OK <text>", false sends "ERR <text>". */
typedef struct {
        bool ok;
        char text[IPC_REPLY_MAX];
} IpcReply;

typedef void (*IpcCmdFn)(void *ctx, const IpcMsg *msg, IpcReply *rep);

/* ---- server (daemon side) ------------------------------------------------- */

/* Creates, binds and listens. `path` NULL = the default per-user location
 * (REV_IPC_SOCK, else $XDG_RUNTIME_DIR/reverie.sock, else
 * $HOME/.cache/reverie/reverie.sock). A live server on the same path is
 * NEVER clobbered: err becomes "in-use" and NULL is returned. The socket
 * file is chmod'd 0600 (owner-only). */
IpcServer *ipc_serve_start(const char *path, char *err, size_t errcap);
void ipc_serve_stop(IpcServer *s);

/* Non-blocking pump: accepts new clients, reads bounded chunks, and runs
 * complete command lines through `fn` (HELLO is handled by the core).
 * Safe to call from the frame loop — work per call is capped. */
void ipc_serve_poll(IpcServer *s, IpcCmdFn fn, void *ctx);

/* The bound socket path (for logging). */
const char *ipc_serve_path(const IpcServer *s);

/* ---- client (revtool / tests) ---------------------------------------------- */

/* Connects to `path` (NULL = default), performs the HELLO handshake, sends
 * one command line, and copies the reply (without the trailing newline)
 * into `reply`. Returns true when the server answered "OK ...". `timeout_ms`
 * bounds connect + each reply. */
bool ipc_request(const char *path, const char *cmdline,
                 char *reply, size_t cap, int timeout_ms);

/* ---- path resolution ------------------------------------------------------- */

/* Fills `buf` with the default socket path (REV_IPC_SOCK / XDG_RUNTIME_DIR /
 * HOME/.cache, creating the directory with 0700 when needed). Returns false
 * with a message when no per-user location exists. */
bool ipc_default_path(char *buf, size_t cap, char *err, size_t errcap);

#endif /* REV_IPC_H */
