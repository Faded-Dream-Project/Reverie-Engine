# Reverie — Autostart (XDG)

Reverie starts on login through the **XDG Autostart** convention: a
`.desktop` entry in `$XDG_CONFIG_HOME/autostart` (default
`~/.config/autostart`). This is the same mechanism most desktop
environments honor on X11 and on Wayland sessions that follow the
freedesktop startup spec — no systemd requirement, no distribution
specifics. Manual launch keeps working everywhere, autostart and manual
launch coexist safely (see the duplicate guard below), and the daemon
itself never writes any of these files: **consent means running
`revtool autostart enable`.**

## Commands

```
revtool autostart enable [--scene PATH | --video PATH | any daemon flags...]
revtool autostart disable
revtool autostart status
```

* `enable` writes two files:
  * `$XDG_CONFIG_HOME/autostart/reverie.desktop` — the entry the session
    manager launches. The `Exec` line is the reverie binary (the revtool
    sibling, found via `/proc/self/exe`; falls back to the plain `reverie`
    name resolved by the session `PATH`), the arguments you gave, and
    `--exit-if-running` appended automatically.
  * `$XDG_CONFIG_HOME/reverie/autostart.json` — the remembered argument
    list. This is the memory of "which wallpaper mode and settings the
    user chose": a later bare `revtool autostart enable` (no arguments)
    re-enables exactly those.
* `disable` removes the `.desktop` entry only; the remembered arguments
  stay so the next enable restores them.
* `status` reports enabled/disabled, both paths, the Exec line, and the
  stored arguments — it never guesses: what it prints is what it read.

Examples:

```
revtool autostart enable --scene ~/.config/reverie/scenes/dawn.rev
revtool autostart enable --video ~/Videos/waves.mp4 --scale fit
revtool autostart enable            # re-enable what was last remembered
revtool autostart status
revtool autostart disable
```

Arguments containing spaces are quoted per the Desktop Entry spec;
`%` and `$` are refused outright (field-code specifiers a session
manager would expand — a wallpaper argument is a path or a flag, never
a specifier).

## The duplicate-instance guard

The generated `Exec` line ends with `--exit-if-running`: at startup the
daemon probes the per-user IPC control socket (the same PING probe
`revtool ipc` uses, honoring `REV_IPC_SOCK`) **before creating any
window**, and exits cleanly when a live daemon answers. A login session
that starts while a manual instance is already running therefore does
not stack a second set of wallpaper windows — the manual instance keeps
the desktop.

Limitations of the guard, stated plainly:

* The probe needs the IPC socket. With `REV_IPC=0` there is no probe
  point; the flag is a no-op in that configuration.
* The probe detects a daemon for the same user and socket path. Two
  daemons deliberately pointed at different sockets (different
  `REV_IPC_SOCK`) are assumed intentional.
* Without the flag, the historical behavior stands: a second daemon
  starts without control of the socket ("in-use" warning) and runs its
  own windows.

## Session-start timing and failures

XDG Autostart entries are executed by the session manager after the
session environment is up (`DISPLAY`/`WAYLAND_DISPLAY` set) — ordering
is the spec's, not Reverie's. Two failure modes are handled explicitly:

* **No display server yet / session without a shell implementation**:
  platform creation fails and the daemon exits with the reason on
  stderr ("wayland: XDG_RUNTIME_DIR is not set", "x11: cannot connect",
  ...). There is no retry loop — a compositor that appears later will
  find no wallpaper; re-run the entry or log in again. (A systemd user
  service with `Restart=on-failure` would add retries; see below.)
* **Compositor without the wallpaper protocols**: Reverie's Wayland
  backend needs `zwlr_layer_shell_v1` (the wlroots-family layer shell)
  — compositors that do not implement it (some GNOME setups) fail at
  connect with a clear error rather than degrading into a normal
  window. On X11 the EWMH `_NET_WM_WINDOW_TYPE_DESKTOP` path is
  universal. The daemon does not assume every Wayland compositor
  supports the same protocol; it reports what it actually found
  (see docs/BACKENDS.md, "Backend selection").

## What is and is not installed

* No systemd unit is installed or required. If you prefer a user
  service (retries, `journalctl` integration), this is a working
  example — save as `~/.config/systemd/user/reverie.service`, then
  `systemctl --user daemon-reload && systemctl --user enable --now reverie`:

  ```ini
  [Unit]
  Description=Reverie wallpaper engine
  After=graphical-session.target

  [Service]
  ExecStart=%h/.local/bin/reverie --scene %h/.config/reverie/scenes/dawn.rev --exit-if-running
  Restart=on-failure
  RestartSec=2

  [Install]
  WantedBy=graphical-session.target
  ```

  (Adjust the paths; the same `--exit-if-running` guard applies.)
* No files are written outside `$XDG_CONFIG_HOME`. Nothing phones home.
* The `.desktop` entry uses only spec keys (`Type`, `Name`, `Comment`,
  `Exec`, `Terminal`) plus the optional `X-GNOME-Autostart-Phase` hint;
  environments that ignore the hint simply start it in the default
  phase.

## Tested vs. manual

Automated (unit, `tests/test_autostart.c`; e2e, `scripts/e2e.sh`):
path resolution, enable writing both files with the quoted Exec line
and the JSON roundtrip, status, disable-keeps-memory, argument restore,
field-code rejection, media-mode arguments, the no-HOME refusal, and —
live under Xvfb — the `--exit-if-running` yield to a running daemon.

Not automatable here (needs a real login session, which CI sandboxes
cannot provide): a session manager actually launching the entry at
login, per-desktop timing differences, and compositor
layer-shell availability. Those steps are manual; report what you see
and the log line names the failing call.
