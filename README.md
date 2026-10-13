# Reverie Engine

**An interactive wallpaper engine written in pure C. The only things it links
are libc, libm, and the Linux kernel — and when a GPU is present, it drives
Vulkan or OpenGL entirely through `dlopen`, with zero build-time
dependencies.**

Reverie is *not* a wallpaper setter — it does not change your desktop
background image. It is the **engine**: it opens real desktop windows behind
your icons (EWMH `_NET_WM_WINDOW_TYPE_DESKTOP` on X11, `zwlr_layer_shell_v1`
on Wayland), renders animated, interactive scenes into them at full frame
rate, and lets you **interact with your wallpaper** — drag your pointer
through a living fluid, trail ripples across water, spark embers with a
click.

Scenes are described in the `.rev` scene language: layered PNGs, SVGs,
gradients, fluid/ripple/particle sims, blend modes, blur, opacity, parallax,
and keyframe animations — all in a small, hand-editable text file, or packed
into a single self-contained `.revpack` binary for distribution.

```
 ~ $ reverie scenes/mountain-dusk.rev
 [info] reverie 0.1.2 — backend=auto renderer=auto
 [info] backend: x11
 [info] renderer: vulkan (tier 3, CPU compositing)
 [info] vulkan: GPU present capable (queue family 0); presenting via platform SHM path
        ... and your desktop is alive.
```

---

## The renderer chain

Reverie probes, in order, and takes the first that works:

```
 vulkan ──► opengl ──► opengl-on-llvmpipe ──► reverie's own CPU engine
```

* **vulkan** — the engine composites on CPU and presents via the GPU: the
  frame is uploaded once to a host-visible staging buffer and copied into
  the swapchain image with image barriers. Driven by `dlopen("libvulkan.so.1")`
  plus a hand-vendored set of declarations verified against the official
  Khronos headers. No Vulkan SDK is needed to build or run.
  Since 0.1.1 the swapchain present is opt-in (`REV_VK_PRESENT=wsi`): the
  default presents through the platform's proven path (MIT-SHM / wl_shm)
  after field reports of driver-specific XWayland black screens with no
  error output. Both paths are E2E-tested and pixel-exact.
* **opengl** — full GPU compositing: layers are uploaded as textures,
  blurred with a downsample + separable gaussian pipeline, and blended with
  W3C blend-mode shaders that mirror the CPU implementation exactly.
  Driven by `dlopen("libEGL.so.1")` + `eglGetProcAddress`; on X11 it needs
  nothing but the raw window ID.
* **opengl on llvmpipe** — not a separate code path: when Mesa reports a
  software rasterizer, the engine logs it and keeps rendering. This is the
  software-GL tier of the chain and works on machines with no GPU at all.
* **cpu** — Reverie's own compositor (SSE-tuned separable blur, W3C blends,
  bilinear resampling). Always available; it is the floor of the chain and
  the reference implementation the GPU shaders are verified against.

Force a tier with `--renderer` or `REV_RENDERER={auto,vulkan,opengl,cpu}`.

## Both display servers, no compromise

* **X11** — hand-written wire protocol over a unix socket: connection setup
  with MIT-MAGIC-COOKIE-1 auth, EWMH desktop hints, MIT-SHM presentation,
  Xinerama monitors. No Xlib, no XCB linkage (the Vulkan path dlopens
  libxcb *at runtime* purely to create its surface — and even that is
  optional).
* **Wayland** — hand-written wire protocol: registry handshake, monitor
  enumeration, `zwlr_layer_shell_v1` background surfaces, double-buffered
  `wl_shm` presentation with premultiplication, pointer events. No
  libwayland-client linkage. GPU-accelerated presentation on Wayland is
  available experimentally via an opt-in WSI shim (`REV_WAYLAND_GPU=1`).

See [docs/BACKENDS.md](docs/BACKENDS.md) for the full matrix, selection
rules, and per-tier notes.

## The absurd dependency list

| Component           | Other engines use   | Reverie uses                          |
|---------------------|---------------------|---------------------------------------|
| Window system       | Xlib / XCB / GTK    | **raw X11 wire protocol over a socket** |
| Wayland             | libwayland-client   | **raw Wayland wire protocol**         |
| GPU rendering       | Vulkan SDK / EGL headers | **dlopen + hand-vendored declarations** |
| Compression         | zlib                | **own DEFLATE inflater (RFC 1951)**   |
| Images              | libpng / libjpeg    | **own PNG codec (all color types, Adam7)** |
| Vectors             | librsvg / cairo     | **own SVG rasterizer + stroker**      |
| Shared memory       | Xext wrappers       | **MIT-SHM via sysv shm, hand-encoded** |
| Scene distribution  | zip / tar           | **own .revpack container (CRC-checked)** |
| Threads             | pthreads            | **single-threaded, zero locks**       |

Everything is C11, compiled with `-Wall -Wextra -Werror -Wpedantic`, and
verified end-to-end under Xvfb:

* an automated test renders frames through the complete pipeline
  (scene → compositor → wire protocol → X server) and reads the pixels back
  with `GetImage` — **byte-identical to the framebuffer**;
* the same screenshot harness runs the daemon on all three GPU tiers
  (llvmpipe EGL and lavapipe Vulkan included) — the Vulkan tier is
  **pixel-exact vs the CPU renderer (mean diff 0.000)**, the GL tier matches
  structurally (residual = the documented box-vs-gaussian blur difference);
* the raw Wayland client is exercised against a hand-written fake
  compositor over a socketpair — registry binds verified byte-exact, shm
  pools mmap'd by the "server", premultiplied pixels checked, pointer
  events and the closed flow included — **and** against a real weston +
  libwayland-server instance (headless): the handshake must complete cleanly
  and the client's first wire bytes are compared byte-for-byte against what
  a real libwayland client sends (`scripts/e2e-wayland.sh`).

## Build

```sh
make            # build/reverie + build/revtool
make test       # run the test suite (inflate, PNG, SVG, scene, pack, wayland)
scripts/e2e.sh  # Xvfb end-to-end, all renderer tiers present on the machine
make assets     # regenerate scene assets using Reverie's own PNG encoder
make install    # /usr/local/bin/{reverie,revtool}
```

Requirements: a C11 compiler, make, and that's it. No headers beyond libc.

## Usage

```sh
reverie [options] <scene.rev | scene.revpack>

  -R, --renderer R    force renderer: auto | vulkan | opengl | cpu
  -B, --backend B     force backend: auto | x11 | wayland
  -f, --fps N         override the scene's target fps
  -m, --monitor N     render only on monitor N
  -o, --once          render one frame to PNG and exit (no display needed)
  -l, --list          list monitors and exit

revtool validate <scene.rev>          # check a scene
revtool render  <scene.rev> ...       # offline PNG rendering (with simulated
                                      #   pointer drags for testing interactions)
revtool pack    <scene.rev> [-o out.revpack]   # bundle scene + assets
revtool unpack  <file.revpack> [-d dir]        # extract it back
```

## Status — 0.1.2 (pre-release)

0.1.2 fixes a field-reported 0.1.1 regression:

* **Layer-shell compositors were mis-detected as lacking `zwlr_layer_shell_v1`**
  ("compositor lacks required globals … falling back to X11") even though the
  global was plainly advertised: the registry's global table had a fixed
  capacity of 48, and the reporting stacking compositor advertises 57 — layer
  shell at #52 and `wl_output` at #57 were silently dropped. The table now
  grows on demand, the failure message reports how many globals were parsed,
  and a flood regression test replays the exact field layout. (The same
  48-entry cap existed in the libwayland WSI shim.)

0.1.1 fixed the two field-reported 0.1.0 regressions:

* **Wayland backend failed on every real compositor** ("protocol parse
  error", then "registry roundtrip timed out"): the wire client encoded the
  message header correctly but *decoded* compositor events with the size and
  opcode bitfields swapped, so the very first server message failed framing.
  Fixed, and the fix is pinned by a byte-exact golden test against real
  libwayland traffic plus a live weston E2E (details in
  [docs/BACKENDS.md](docs/BACKENDS.md)).
* **Black screen when falling back to X11 on Wayland sessions**: the vulkan
  tier presented through its WSI swapchain, which some real-driver XWayland
  stacks silently drop (no error anywhere). 0.1.1 defaults the tier to the
  platform's proven presenter (MIT-SHM / wl_shm); `REV_VK_PRESENT=wsi`
  restores the swapchain path. Both are pixel-exact vs the CPU renderer.

The 0.x series exists to prove the architecture on real hardware before any
1.0 API freeze. Core module test coverage: DEFLATE/PNG codec, SVG stack,
scene language, pack container, Wayland wire client — all green; X11 E2E
pixel-exact; renderer chain E2E on llvmpipe + lavapipe; Wayland E2E against
real weston. The 0.2 roadmap: Vulkan compute compositing (blurring and
blending with shaders instead of present-only), the Wayland WSI shim
graduating from experimental, more SVG filters.

## Repository layout

```
src/          engine sources (see docs/ARCHITECTURE.md)
scenes/       example .rev scenes + hand-drawn assets
tests/        unit + integration suites (incl. the Wayland fake compositor)
scripts/e2e.sh         Xvfb end-to-end, all renderer tiers present on the machine
scripts/e2e-wayland.sh real-weston Wayland wire-protocol E2E (needs the test stack)
docs/         FORMAT (scene language + .revpack), ARCHITECTURE, BACKENDS
```

## License

Proprietary — All Rights Reserved. Copyright (c) 2026 FemBoyGamerTechGuy.
See [LICENSE](LICENSE). No use, copying, modification, or distribution
without written permission from the copyright holder.
