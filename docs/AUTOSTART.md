# Reverie — Autostart (XDG) and `--restore`

Reverie starts on login through the **XDG Autostart** convention: a
`.desktop` entry in `$XDG_CONFIG_HOME/autostart` (default
`~/.config/autostart`). This is the same mechanism most desktop
environments honor on X11 and on Wayland sessions that follow the
freedesktop startup spec — no systemd requirement, no distribution
specifics. Manual launch keeps working everywhere, autostart and manual
launch coexist safely (see the duplicate guard below), and the daemon
itself never writes any of these files: **consent means running
`revtool autostart enable`.**

## The model: autostart = restore

Autostart does not remember anything of its own. The entry it writes is
exactly:

```
Exec=<reverie binary> --restore --exit-if-running
```

`reverie --restore` launches **whatever was launched last** — theme
folder, `.rev` scene, `.revpack`, `--video` file, or `--image` file.
The daemon keeps that memory itself: every successful content launch
records the wallpaper-defining subset of its command line (content path
made **absolute**, plus `--scale`/`--no-loop`/`--fps`/`--monitor`/
`--renderer`/`--backend` when they were given) into

```
$XDG_STATE_HOME/reverie/last-session.json     (default
~/.local/state/reverie/last-session.json)
```

Runtime theme switches update the same record — switch fifty times over
a session via `revtool ipc THEME`, and the next login restores the one
you actually ended up on. Because the daemon owns the memory and
autostart only replays it, "start on login" and "what was last
launched" can never drift apart — there is no second list of arguments
to go stale.

Deliberate semantics, stated plainly:

* The record replays the **command line**, not the environment:
  `REV_RENDERER=cpu reverie themes/x` does not freeze `--renderer cpu`
  into the record — the env var applies again at restore time if it is
  still set.
* `--once` screenshot runs are tool invocations, not wallpaper
  launches: they never touch the memory.
* A restoring invocation may still spell out flags — explicit wins:
  `reverie --restore --renderer cpu` restores the content but forces
  the CPU renderer. Only the content slot is exclusively the record's;
  combining `--restore` with `--scene`/`--video`/`--image`/a positional
  is a hard error (ambiguous, therefore refused).
* The record is re-parsed through the real argument parser at every
  restore: a corrupt or hand-edited file can only fail with a named
  error, never inject behavior a typed command line could not produce.
  A record containing `--restore` itself is rejected (no replay loops).
* Nothing recorded is ever executed by a shell — the `.desktop` Exec
  line is the fixed pair of flags above.

## Commands

```
revtool autostart enable      # write the entry (no arguments — ever)
revtool autostart disable     # remove the entry
revtool autostart status      # entry state + what --restore would replay
```

* `enable` writes the single `.desktop` entry. The `Exec` line is the
  reverie binary (the revtool sibling, found via `/proc/self/exe`;
  falls back to the plain `reverie` name resolved by the session
  `PATH`), `--restore`, and `--exit-if-running`. It takes NO arguments:
  autostart restores the last session instead of pinning one. If you
  want autostart to start a specific wallpaper, run reverie with it
  once, then `revtool autostart enable`.
* `disable` removes the `.desktop` entry and also deletes the legacy
  `~/.config/reverie/autostart.json` (the argument memory of the older
  mechanism, kept only to be cleaned up).
* `status` reports enabled/disabled, the entry path, the Exec line, and
  what `--restore` would replay right now (the session record — or an
  honest "nothing recorded yet").

Examples:

```
./build/reverie themes/moonlit-reverie    # launch what you like
revtool autostart enable                  # start THAT on every login
./build/reverie --video ~/vids/waves.mp4 --scale fit
revtool autostart status                  # restores: --video ... --scale fit
revtool autostart disable                 # off
reverie --restore                         # manual: same replay, right now
```

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
* **Nothing ever launched** (`--restore` with no session record): the
  daemon exits with `reverie: nothing to restore — no previous session
  recorded (<path>)`. Run reverie once with any theme, video, or image,
  and the record exists from then on.
* **The recorded file was deleted or moved**: the normal content-load
  error names the path (recorded absolute, so the message says exactly
  which file is gone). The record is NOT cleared by the failure — fix
  the file or launch something else to overwrite it.
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
  ExecStart=%h/.local/bin/reverie --restore --exit-if-running
  Restart=on-failure
  RestartSec=2

  [Install]
  WantedBy=graphical-session.target
  ```

  (Adjust the path; the same `--exit-if-running` guard and the same
  session memory apply — the unit and the .desktop entry are
  interchangeable launchers.)
* No files are written outside `$XDG_CONFIG_HOME` by revtool, or
  outside `$XDG_STATE_HOME` by the daemon. Nothing phones home.
* The `.desktop` entry uses only spec keys (`Type`, `Name`, `Comment`,
  `Exec`, `Terminal`) plus the optional `X-GNOME-Autostart-Phase` hint;
  environments that ignore the hint simply start it in the default
  phase.

## Migration from the old mechanism

Releases before `--restore` stored an argument list in
`$XDG_CONFIG_HOME/reverie/autostart.json` and pinned it in the Exec
line. Such an entry keeps working as-is (it is still a valid command
line) until you re-run `revtool autostart enable`, which replaces it
with the `--restore` entry; `revtool autostart disable` deletes the
leftover JSON. There is no automatic migration: the old pin and the
new memory are different behaviors, and the switch is one explicit
command.

## Tested vs. manual

Automated (unit: tests/test_session.c, tests/test_autostart.c,
tests/test_json.c, tests/test_cli.c; e2e: scripts/e2e.sh): the record's
location and format, absolute-path recording, tool-flag exclusion, the
restore merge (record fills, explicit current flags win), hostile and
corrupt records rejected through the real parser (including a record
containing `--restore`), runtime-switch content updates, the
enable/disable/status lifecycle, the `--restore` Exec contract, legacy
cleanup, and — live under Xvfb — a full record/replay roundtrip and the
IPC-switch memory update. The JSON parser growth bug this work
exposed (>8-element containers lost their first entries) is pinned by
tests/test_json.c.

Not automatable here (needs a real login session, which CI sandboxes
cannot provide): a session manager actually launching the entry at
login, per-desktop timing differences, and compositor
layer-shell availability. Those steps are manual; report what you see
and the log line names the failing call.
