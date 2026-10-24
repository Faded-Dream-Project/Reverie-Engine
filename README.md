# Reverie
**Part of the Faded Dream Project**

Reverie is a live-wallpaper engine for modern Linux desktops, developed
as part of the
[Faded Dream Project](https://github.com/FemBoyGamerTechGuy/Faded-Dream-dotfiles).
[![License: Reverie Proprietary v1.0](https://img.shields.io/badge/license-Reverie%20Proprietary%20v1.0-blueviolet?style=flat-square)](LICENSE)
[![Built with C](https://img.shields.io/badge/built%20with-C-orange?style=flat-square&logo=c)](https://www.open-std.org/jtc1/sc22/wg14/)
[![Standard: C11](https://img.shields.io/badge/standard-C11-orange?style=flat-square&logo=c)](https://www.open-std.org/jtc1/sc22/wg14/)
[![Backends: X11 and Wayland](https://img.shields.io/badge/backends-X11%20%7C%20Wayland-blueviolet?style=flat-square)](docs/BACKENDS.md)
[![Version: 0.1.0](https://img.shields.io/badge/version-0.1.0-blueviolet?style=flat-square)](CHANGELOG.md)
[![Part of Faded Dream](https://img.shields.io/badge/part%20of-Faded%20Dream-purple?style=flat-square)](https://github.com/FemBoyGamerTechGuy/Faded-Dream-dotfiles)
---
Reverie is *not* a wallpaper setter — it does not change your desktop
background image. It is the **engine**: it opens real desktop windows
behind your icons (EWMH `_NET_WM_WINDOW_TYPE_DESKTOP` on X11,
`zwlr_layer_shell_v1` on Wayland), renders animated, interactive scenes
into them at full frame rate, and lets you **interact with your
wallpaper** — drag your pointer through a living fluid, trail ripples
across water, spark embers with a click.

The only things it links are libc, libm, and the Linux kernel — and
when a GPU is present, it drives Vulkan or OpenGL entirely through
`dlopen`, with zero build-time dependencies. X11 and Wayland are both
spoken in hand-written wire protocol over a socket.
**Status:** **v0.1.0 released** — the first public release. The engine,
the `.rev` scene language, both display backends, the three-tier
renderer chain (Vulkan compute → OpenGL → native CPU), video and
static-image wallpaper modes, IPC runtime control, session restore and
autostart, and the cross-distribution packages are all in. The
release history lives in [`CHANGELOG.md`](CHANGELOG.md).
Reverie is **distro-agnostic**: it builds and runs on any modern Linux
distribution where X11, Wayland, and POSIX are available.

## Features

- **Three renderer tiers, one engine** — hardware Vulkan (full GPU
  compute compositing: every layer op runs as a SPIR-V compute kernel),
  hardware OpenGL (GPU compositing + GPU fluid/ripple solvers), and
  Reverie's own CPU renderer (SSE2 baseline, AVX2 runtime dispatch).
  `auto` picks the best present stack deterministically; explicit
  requests are honored exactly, never silently substituted.
- **Real X11 and Wayland backends, hand-written** — raw wire protocol
  over a unix socket for both. No Xlib, no XCB linkage, no
  libwayland-client linkage (the GPU Wayland path dlopens it at run
  time, and falls back to the raw client when absent).
- **The `.rev` scene language** — layered images, gradients, solids,
  fluid, ripple and particle simulations, video, blend modes, blur,
  opacity, parallax, monitor selection, and keyframe animations in a
  small, hand-editable text file. Image layers composite smaller images
  at authored rects (`pos`/`size`/`scale`/`anchor`, all animatable) —
  drifting sprites, tiled panels, pixel-perfect placements.
- **Themes** — self-contained folders (`theme.json` + scene + assets)
  or single-file packed `.revpack` themes loaded without extraction,
  byte-identical renders either way. Hot-reload while the daemon runs:
  edit a scene or swap an asset and the desktop updates.
- **Video and image wallpapers** — `reverie --video clip.mp4` (FFmpeg
  libraries dlopen'd at runtime — absent means no video, nothing else
  changes) and `reverie --image wall.png` (Reverie's own native PNG
  codec — no FFmpeg anywhere, works in every build). Shared placement
  vocabulary: `--scale fill|fit|stretch|center`, or the valueless
  `--fill --fit --stretch --center`.
- **Runtime control** — a per-user IPC socket drives the running
  daemon: `revtool ipc "THEME themes/snowfall 800"` switches with a
  dip-through-black fade, `PLAYLIST`/`ROTATE` run unattended rotation,
  `STATUS`/`QUIT` round it out.
- **Autostart = restore** — one command, no arguments to drift stale:
  `reverie --restore` replays whatever was launched last — a `.rev`
  theme, a video, or an image — from the daemon's own session memory.
  `revtool autostart enable` installs exactly that as the login entry.
- **Idle power-save** — rendering pauses when nothing is shown
  (fullscreen app over the wallpaper, display asleep) and resumes on
  evidence, with the scene clock frozen through the gap so animations
  resume exactly where they stopped.
- **A dependency list of one** — see the table below.

## Requirements

- A C11 compiler and `make` (developed against GCC; see
  [docs/BUILD-DISTRO.md](docs/BUILD-DISTRO.md) for per-distribution
  package names)
- Optional: FFmpeg development headers for video wallpaper support
  (`libavcodec-dev libavformat-dev libavutil-dev libswscale-dev` —
  distro names vary). Without them the engine builds as the no-video
  stub; `--video` then refuses with a clear message. Video is never
  linked — the libraries are dlopen'd at runtime with an ABI guard.
- Optional: `Xvfb` to run the X11 end-to-end suite without a desktop
  session

## Installation

Binary packages (`.deb`, `.rpm`, `.pkg.tar.zst`, `.apk`, `.xbps`) for
x86_64 are published on the
[releases page](https://github.com/FemBoyGamerTechGuy/Reverie-Engine/releases)
and install with each distribution's own package manager — Debian 12/13
and Ubuntu 24.04+, the RHEL 10 family and Fedora 42+, Arch, Alpine 3.22
(musl), and Void (glibc):

```sh
sudo apt  install ./reverie_0.1.0_amd64.deb        # Debian, Ubuntu
sudo dnf  install reverie-0.1.0-1.x86_64.rpm        # Fedora, RHEL
sudo zypper install reverie-0.1.0-1.x86_64.rpm      # openSUSE
sudo pacman -U reverie-0.1.0-1-x86_64.pkg.tar.zst   # Arch
sudo apk  add --allow-untrusted reverie-0.1.0-r0.apk # Alpine (local file)
sudo xbps-install -R . reverie-0.1.0_1.xbps         # Void
```

Each package installs both binaries — `reverie` (the engine) and
`revtool` (the scene toolkit and IPC client) — plus the documentation.
Everything else builds from source (below).

## Building

```sh
make            # build/reverie + build/revtool
make test       # the full unit + integration suite (no display needed)
scripts/e2e.sh  # Xvfb end-to-end, every renderer tier present
make assets     # regenerate theme assets with Reverie's own PNG encoder
make install    # PREFIX/bin/{reverie,revtool} — binaries only
```

`make install` never ships the bundled themes: a wallpaper you did not
ask for has no business landing on your disk. Install a theme the same
way you got it — copy the folder, or a `.revpack`, anywhere you like
(themes are self-contained; there is no registry and no install step).
`make install-themes` exists for packagers who explicitly want the
collection.

```sh
make                      # auto: system FFmpeg headers, else the vendored
                          # contrib headers, else the no-video stub
make ENABLE_FFMPEG=0      # no-video stub build — everything else identical
make ENABLE_FFMPEG=1      # video required — missing headers are a hard error
```

## The renderer policy

**Explicit requests are honored exactly** — `--renderer
vulkan|opengl|cpu` initializes that tier and nothing else; failure
exits with the reason instead of silently substituting a different
renderer. Software devices (lavapipe Vulkan, llvmpipe GL) are used
when explicitly requested and reported as software in every log line.

**Auto is deterministic:**

```
 hardware vulkan ──► hardware opengl ──► reverie's own CPU engine
        (llvmpipe and lavapipe are opt-in:
         REV_AUTO_GL_SW=1 / REV_AUTO_VK=1 — measured rationale in
         docs/BACKENDS.md: neither software path is uniformly faster)
```

## The dependency list

| Component           | Other engines use   | Reverie uses                          |
|---------------------|---------------------|---------------------------------------|
| Window system       | Xlib / XCB / GTK    | **raw X11 wire protocol over a socket** |
| Wayland             | libwayland-client   | **raw Wayland wire protocol**         |
| GPU rendering       | Vulkan SDK / EGL headers | **dlopen + hand-vendored declarations** |
| Compression         | zlib                | **own DEFLATE inflater (RFC 1951)**   |
| Images              | libpng / libjpeg    | **own PNG codec (all color types, Adam7)** |
| Vectors             | librsvg / cairo     | **own SVG rasterizer + stroker**      |
| Shared memory       | Xext wrappers       | **MIT-SHM via sysv shm, hand-encoded** |
| Scene distribution  | zip / tar           | **own .revpack container (CRC-checked, path-validated)** |
| Video (optional)    | linked FFmpeg / GStreamer | **dlopen'd libav\* — absent = no video, nothing else changes** |
| Threads             | pthreads            | **one optional decode thread (video); everything else lock-free** |

Everything is C11, compiled with `-Wall -Wextra -Werror -Wpedantic`,
and verified end-to-end: automated renders read back through the X
server **byte-identical to the framebuffer**, the Vulkan tier is
**pixel-exact vs the CPU renderer (mean diff 0.000)**, and the Wayland
wire bytes are compared against what a real libwayland client sends.

## Usage

```sh
reverie themes/ripple-lake            # folder theme (hot-reloads)
reverie ripple-lake.revpack           # packed theme — no extraction
reverie --video loops/waves.mp4       # media mode: video/GIF wallpaper
reverie --video loop.gif --scale fit  # letterbox instead of cover-crop
reverie --video --stretch waves.mp4   # the same, as a valueless flag
reverie --image walls/dusk.png        # static media mode: one PNG — the
                                      #   engine's own codec, no FFmpeg
reverie --restore                     # whatever was launched last — the
                                      #   daemon remembers (absolute path,
                                      #   runtime THEME switches included)
reverie --restore --renderer cpu      # restore the content, override a flag
```

Scene layers: image, gradient, solid, fluid, ripple, particles, video —
with blend modes, blur, opacity, parallax, monitor selection, and
keyframe animations. Image layers composite smaller images at authored
rects (pos/size/scale/anchor, animatable — drifting sprites, tiled
panels, pixel-perfect placements). Video layers: `type = video; src =
"loop.mp4"; loop = true; paused = false; fit = fill`.

While the daemon runs it accepts commands on a per-user control
socket: `revtool ipc "PLAYLIST themes/ripple-lake themes/snowfall"`,
`revtool ipc "ROTATE 900"`, `revtool ipc "THEME themes/ember-drift
800"` — switches dip through black with a fade, playlists rotate
unattended, and idle power-save keeps working across switches.

```sh
revtool validate <scene.rev | theme>  # check a scene or theme
revtool render  <scene.rev> ...       # offline PNG rendering
revtool pack    <scene.rev> [-o out.revpack]   # bundle scene + assets
revtool unpack  <file.revpack> [-d dir]        # extract it back
revtool theme   list|info|validate|pack|inspect
revtool ipc     "NEXT 800"           # talk to the RUNNING daemon
revtool autostart enable             # XDG Autostart at login — see below
```

## Autostart

The only autostart entry is **`reverie --restore`** — nothing else.
Autostart does not remember anything of its own: the daemon keeps the
session memory (`~/.local/state/reverie/last-session.json`, updated by
every launch and runtime theme switch), and the login entry simply
replays it. Whatever you ran last — a `.rev` theme, a packed theme, a
video, or an image — is what comes back at login. "Start on login" and
"what was last launched" can never drift apart, because there is no
second list of arguments to go stale.

`revtool autostart enable` installs the entry (opt-in, user-controlled,
in `~/.config/autostart/`); `disable` removes it. Full semantics —
including the duplicate-instance guard and what the record replays —
in [docs/AUTOSTART.md](docs/AUTOSTART.md).

## What it looks like

Real captured frames from the bundled themes (offline render at
1920×1080, CPU tier):

![mountain-dusk](docs/img/mountain-dusk.png)
![fluid-aurora](docs/img/fluid-aurora.png)
![ember-drift](docs/img/ember-drift.png)

## Documentation

| Doc | Covers |
|---|---|
| [`docs/SCENE-FORMAT.md`](docs/SCENE-FORMAT.md) | The `.rev` scene language and layer vocabulary |
| [`docs/THEMES.md`](docs/THEMES.md) | Theme folders, `theme.json`, `.revpack` packages |
| [`docs/BACKENDS.md`](docs/BACKENDS.md) | X11/Wayland backends, renderer tiers, GPU paths, env knobs |
| [`docs/VIDEO.md`](docs/VIDEO.md) | Video wallpapers and the media mode |
| [`docs/IMAGE.md`](docs/IMAGE.md) | Static image wallpapers and placement |
| [`docs/AUTOSTART.md`](docs/AUTOSTART.md) | Autostart, `--restore`, the session record |
| [`docs/IPC.md`](docs/IPC.md) | The runtime control socket and `revtool ipc` |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Layering, module boundaries, data flow |
| [`docs/PNG.md`](docs/PNG.md) | The native PNG codec |
| [`docs/BUILD-DISTRO.md`](docs/BUILD-DISTRO.md) | Per-distribution build packages and quirks |
| [`CHANGELOG.md`](CHANGELOG.md) | Release history |

## Project principles

- **No display-server library, no wrapping either.** X11 and Wayland
  are spoken in raw wire protocol; the GPU stacks are reached through
  `dlopen` with hand-vendored declarations.
- **Zero build-time dependencies beyond libc/libm** — every table row
  above is an own implementation, justified by a measured need.
- **Correct over quick; pixel evidence over claims** — the test suites
  read pixels back through the real display server and compare.
- **No fake completion** — known limits are documented per feature,
  in the docs, not hidden.

## License

Reverie is **proprietary software**. The engine — its source code,
tools, and shipped assets — remains the exclusive property of its
rights holder and may not be copied, modified, redistributed,
sublicensed, published, or commercially exploited without express
written authorization. See [`LICENSE`](LICENSE) (the Reverie
Proprietary License, version 1.0; governed by the laws of Romania,
with exclusive venue in Bucharest).

**Wallpapers you create are yours.** The license governs the engine,
not the `.rev` scenes, themes, or media wallpapers you build with it:
your wallpaper's files, assets, branding, and licensing stay your
independent property, and you may distribute and sell them under terms
of your choosing. The engine's restrictions never attach to your
wallpaper — ownership runs one way.

**The engine is installed separately.** Wallpaper you distribute must
not bundle the Reverie binaries: end users obtain and install the
engine themselves — from the release packages or a build of this
repository. Requiring the engine does not diminish your wallpaper's
independence or distributability.
