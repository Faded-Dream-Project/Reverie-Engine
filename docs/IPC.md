# Reverie runtime control (IPC)

A running daemon exposes a small, versioned, text-line control socket so
themes can be switched, playlists rotated, and the engine stopped — without
restarting and without losing the session (windows, renderer, GPU state).

`revtool ipc` is the reference client; anything that can write a unix socket
works (`socat`, `nc -U`, a shell loop).

## The socket

| | |
|---|---|
| path | `$REV_IPC_SOCK`, else `$XDG_RUNTIME_DIR/reverie.sock`, else `$HOME/.cache/reverie/reverie.sock` |
| permissions | `0600`, owner-only; `SO_PEERCRED` refuses any other uid |
| disabled by | `REV_IPC=0` |
| collisions | a live daemon's socket is never clobbered — the second instance logs `in-use` and runs without control; a stale socket from a crashed daemon is reclaimed |
| shutdown | the socket is unlinked on clean exit |

The protocol is newline-terminated ASCII, one reply per command. Clients
must handshake first. Every value is bounded (line ≤ 4096 bytes, ≤ 16
arguments, ≤ 8 clients), malformed input gets an `ERR` (binary garbage drops
the connection), and the daemon never blocks on the socket — it pumps from
its frame loop with a per-poll work cap. A hung-up client mid-reply cannot
crash the daemon (writes use `MSG_NOSIGNAL`).

## Commands (protocol v1)

```
HELLO 1                          -> OK 1                     (handshake, required)
PING                             -> OK PONG
STATUS                           -> OK theme=... fps=... renderer=... backend=...
                                     visible=... fade=... playlist=... rotate=...
THEME <path> [fade_ms]           -> OK switched | ERR <reason>
PLAYLIST <path> [<path> ...]     -> OK playlist N entries    (up to 64)
NEXT [fade_ms]                   -> OK <path> | ERR no-playlist
PREV [fade_ms]                   -> OK <path> | ERR no-playlist
ROTATE <seconds|0>               -> OK rotate=<s>            (0 = off, max 86400)
FADE <ms>                        -> OK fade=<ms>             (default for switches)
QUIT                             -> OK bye                   (clean shutdown)
```

`<path>` is a theme directory, a `.revpack`, or a plain `.rev` scene —
resolved by the SAME loader the CLI uses (theme engine-version gate, asset
provider, everything). A failed load answers `ERR` and changes nothing:
the daemon keeps rendering whatever it had.

## Fades

Theme switches **dip through black** by default (400 ms each way,
`REV_FADE_MS` or the `FADE` command): the new scene loads first — one
short stall while the old one still shows — then a black overlay fades in
over the live frame, the scene swaps at full darkness, and the overlay
fades back out. This works identically on every backend: the CPU tier
darkens the finished frame (`rev_comp_fade`, SSE2), the GPU tiers composite
one constant-black layer stretched over the frame (a 1x1 texture uploaded
once per transition). A switch that arrives mid-fade continues from the
current darkness — the obsolete target never flashes in.

While the display is idle (power-save) a fade still advances on wall-clock
time; the scene swaps invisibly and the new theme is what wakes up. Fades
cost nothing once finished. A true cross-fade (two live scenes blended)
needs an offscreen composite on the GPU tiers and is future work.

## Playlists and rotation

`PLAYLIST` stores up to 64 paths; `NEXT`/`PREV` wrap around; `ROTATE <sec>`
auto-advances every N seconds (works with the fade defaults). A playlist
entry that fails to load is skipped with a warning — rotation never wedges.
The rotation and the file hot-reload watcher coexist: switching re-arms the
watcher on the new theme (its snapshot is taken at load), so editing the
CURRENT theme's files still hot-reloads.

## Examples

```
$ revtool ipc PING
OK PONG

$ revtool ipc STATUS
OK theme=themes/ripple-lake fps=60 renderer=vulkan backend=wayland visible=1 fade=400 playlist=0 rotate=0

$ revtool ipc "PLAYLIST themes/ripple-lake themes/snowfall themes/ember-drift"
OK playlist 3 entries

$ revtool ipc "ROTATE 900"        # every 15 minutes, with the default fade
OK rotate=900

$ revtool ipc "NEXT 1200"         # skip now, slower fade
OK themes/snowfall

$ revtool ipc QUIT
OK bye
```

`revtool ipc` exits 0 on `OK`, 1 on `ERR` — playlist scripts can branch on it.

## Security notes

The socket is a per-user resource: the path lives in a per-user directory,
the file mode is `0600`, and the server re-checks the peer's uid via
`SO_PEERCRED` before accepting commands. There is no command execution, no
shell, and no path is touched that the theme loader would not touch anyway
(the CLI has exactly the same power). The protocol intentionally cannot
read files or report their contents — `STATUS` reports paths and counters
only.
