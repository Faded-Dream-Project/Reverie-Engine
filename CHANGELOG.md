# Changelog

All notable changes to Reverie are documented here. The format follows
the project's house style: every entry states what changed, why, and
how it was proven.

## 0.1.0 — 2026-10-24

The first public release: the engine, the scene language, both display
backends, the three-tier renderer chain, media wallpapers, runtime
control, session restore, and cross-distribution packages.

### Engine

- **Three renderer tiers behind one interface** — hardware Vulkan
  (tier 3: every layer operation — blend, blur, gradients, particles,
  video, fluid/ripple solvers — runs as a SPIR-V compute kernel, with
  GPU timer queries and host-cached frame download), hardware OpenGL
  (tier 2: GPU compositing plus GPU fluid/ripple solvers on RGBA16F
  targets), and the native CPU renderer (tier 1: SSE2 baseline, AVX2
  runtime dispatch). All GPU stacks are reached through `dlopen` with
  hand-vendored declarations — zero build-time dependencies. `auto`
  picks deterministically (hardware Vulkan → hardware OpenGL → CPU);
  explicit requests are honored exactly, failures exit with the reason.
- **Raw X11 backend** — hand-written wire protocol over a unix socket:
  connection setup with MIT-MAGIC-COOKIE-1 authentication, EWMH
  desktop-window hints, MIT-SHM presentation, Xinerama monitor
  enumeration, DPMS-aware idle power-save, and Expose-driven repair so
  static content snaps back instantly when a WM workspace switch
  uncovers it.
- **Raw Wayland backend** — hand-written wire protocol: registry
  handshake, `zwlr_layer_shell_v1` background surfaces, double-buffered
  `wl_shm` presentation with premultiplication and BGRA byte order,
  pointer events, output hotplug. The GPU-present shim dlopens
  libwayland-client at run time and falls back to the raw client when
  absent. Idle power-save rides frame-callback evidence with a
  debounced hidden threshold — slow-paced surfaces (static images pace
  at 2 fps) stay provably visible without flapping.
- **Output hotplug** — monitors come and go while the daemon runs; the
  window set is reconciled with the monitor list, matched by stable
  identity, with mid-run surface creation and release.

### Scene language and themes

- **The `.rev` scene language** — layered image, gradient, solid,
  fluid, ripple, particle, and video layers with blend modes (W3C
  separable), premultiplied box blur, opacity, parallax, monitor
  selection, and keyframe animations (9 easings, once/loop/pingpong).
- **Image layer placement** — `pos`/`size`/`scale`/`anchor` (all
  animatable), tile mode, and pixel-perfect placement: composite many
  small images into one big frame, drift sprites on keyframes, tile
  panels — pure data, no engine code.
- **Themes** — self-contained folders (`theme.json` + scene + assets)
  and single-file `.revpack` containers (CRC-checked,
  path-validated), byte-identical renders either way; hot-reload
  watches every file under the theme.
- **Bundled themes** — mountain-dusk, fluid-aurora, ember-drift,
  ripple-lake, snowfall, light-lattice, nebula-drift.

### Media wallpapers

- **`--video`** — video and animated-GIF wallpapers through dlopen'd
  FFmpeg (software decode, GPU color convert on the GPU tiers), with a
  runtime ABI guard. Absent libraries refuse video cleanly; the
  no-video stub build is otherwise identical. Shared placement
  vocabulary `--scale fill|fit|stretch|center` plus valueless
  shorthands `--fill/--fit/--stretch/--center` that may sit between
  the content flag and its path. `--no-loop` holds the last frame at
  end-of-file.
- **`--image`** — static PNG wallpapers through Reverie's own native
  codec (every color type, bit depth, and interlacing), no FFmpeg
  anywhere in the path, works in every build configuration. Same
  placement vocabulary; GPU tiers upload the texture once and rebind.

### Runtime control and session

- **IPC control socket** — per-user, versioned, `SO_PEERCRED`-checked:
  `THEME` (with dip-through-black fade), `PLAYLIST`/`NEXT`/`PREV`/
  `ROTATE` unattended rotation, `STATUS`, `PING`, `QUIT`. Media mode
  answers theme commands with a clear error instead of pretending.
- **Session restore and autostart** — every successful launch records
  the wallpaper-defining subset of its command line
  (`~/.local/state/reverie/last-session.json`, absolute paths, runtime
  theme switches included); `reverie --restore` replays it — theme,
  pack, video, or image. The XDG autostart entry
  (`revtool autostart enable`) is exactly `reverie --restore
  --exit-if-running`: one command, nothing to drift stale.
- **Idle power-save** — rendering pauses when nothing is shown and
  resumes on latch evidence, with the scene clock frozen through the
  gap; an idle watchdog audits long-hidden periods on every platform.

### Tooling, tests, packaging

- **revtool** — validate, render (offline PNG with simulated pointer
  interaction), pack/unpack, theme toolkit, `ipc` client, autostart
  management.
- **Test suites** — 21 unit/integration suites (inflate, PNG, SVG,
  geometry, scene, pack, wayland fake-compositor, wlshim with real
  libwayland, IPC, autostart, session, CLI, X11 events, ...), the
  Xvfb end-to-end suite (60 legs: every renderer tier, interactions
  verified by pixel centroid, IPC, restore for theme/video/image,
  workspace-switch occlusion repair), and the weston headless Wayland
  E2E. ASan+UBSan clean with leak detection.
- **Cross-distribution packages** — `.deb`, `.rpm`, `.pkg.tar.zst`,
  `.apk`, `.xbps` built by CI on Debian, Fedora, openSUSE, Arch,
  Alpine, and Void containers; both binaries (`reverie` and `revtool`)
  in every package.

### Fixed during the release cycle (field reports)

- Wayland event decode swapped size/opcode — "protocol parse error" /
  "registry roundtrip timed out" on real compositors.
- Black screen on XWayland-fallback sessions.
- GPU-path presentation wedges (acquire stalls, backpressure mistaken
  for visibility) — latch evidence is frame callbacks only.
- wl_shm BGRA byte order (blues rendered red).
- Staging-readback per-pixel loops over uncached GPU memory
  (1.1 s/frame on a GTX 1050 Ti → one memcpy).
- Theme hot-reload watched the folder mtime, which never moves on
  in-place edits — now snapshots every file.
- X11 workspace switches stranded static content until the next tick
  (openbox) — Expose now re-presents immediately.
- `--video --stretch clip.mp4` regressed to a usage error — the
  valueless shorthands defer the content path again.
- Idle visibility flapped ~2 Hz on slow-paced (static image)
  surfaces — the hidden decision is now debounced and probes keep
  evidence alive during pacing sleeps; transition logs dampen if a
  compositor ever oscillates anyway.
- Illustrator-style SVG paths (`M0,90 L14,44` — comma separators)
  silently rendered nothing.
