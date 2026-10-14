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
gradients, fluid/ripple/particle simulations, MP4 video, blend modes,
blur, opacity, parallax, and keyframe animations — all in a small,
hand-editable text file. Scenes ship as **themes**: self-contained folders
(`theme.json` + scene + assets) or single-file packed `.revpack` themes
loaded without extraction — byte-identical renders either way.

```
 ~ $ reverie scenes/mountain-dusk.rev
 [info] reverie 0.2.0-dev — backend=auto renderer=auto
 [info] backend: x11
 [info] opengl: hardware renderer NVIDIA GeForce GTX 1050 Ti/PCIe/SSE2 (vendor: NVIDIA)
 [info] opengl: fluid + ripple solvers on GPU (RGBA16F render targets)
 [info] renderer: opengl (tier 2, GPU compositing) — NVIDIA GeForce GTX 1050 Ti/PCIe/SSE2 [hardware]
        ... and your desktop is alive.
```

---

## The renderer policy

**Explicit requests are honored exactly** — `--renderer vulkan|opengl|cpu`
initializes that tier and nothing else; failure exits with the reason
instead of silently substituting a different renderer. Software devices
(lavapipe Vulkan, llvmpipe GL) are used when explicitly requested and
reported as `SOFTWARE` in every log line.

**Auto is deterministic** (2026-10 redesign — the old
`vulkan → opengl → llvmpipe → cpu` cascade initialized a present-only
Vulkan tier ahead of the actually-accelerated GL tier and, on fallback
machines, booted an entire software GL stack to avoid the native CPU
renderer):

```
 hardware opengl ──► reverie's own CPU engine
        (llvmpipe and the present-only vulkan tier are opt-in:
         REV_AUTO_GL_SW=1 / REV_AUTO_VK=1 — measured rationale in
         docs/BACKENDS.md: neither software path is uniformly faster)
```

* **vulkan** — PRESENT-ONLY today: the engine composites on CPU and the GPU
  receives finished frames — uploaded to a host-visible staging buffer and
  copied into the swapchain image with image barriers. Driven by
  `dlopen("libvulkan.so.1")` plus a hand-vendored set of declarations
  verified against the official Khronos headers. No Vulkan SDK is needed
  to build or run. Device selection enumerates every physical device and
  picks by hardware class (discrete > integrated > virtual > CPU), logging
  the selected device name/type/driver and an honest hardware/software
  verdict — initialization success is never conflated with acceleration.
  Since 0.1.1 the swapchain present is opt-in (`REV_VK_PRESENT=wsi`): the
  default presents through the platform's proven path (MIT-SHM / wl_shm)
  after field reports of driver-specific XWayland black screens with no
  error output. Both paths are E2E-tested and pixel-exact. Because this
  tier does no GPU rendering, `auto` does not select it (opt in with
  `REV_AUTO_VK=1`).
* **opengl** — full GPU compositing **and GPU simulation**: layers are
  uploaded as textures (static rasters only re-upload when their content
  actually changes), blurred with a downsample + separable gaussian
  pipeline, and blended with W3C blend-mode shaders that mirror the CPU
  implementation exactly. Fluid layers run their ENTIRE simulation on the
  GPU — splats, vorticity confinement, Jacobi pressure projection,
  semi-Lagrangian advection and the dye tonemap all execute as fragment
  shaders over RGBA16F ping-pong textures. Ripples run their wave equation
  in shaders and composite their refraction against the accumulation
  target. Particle layers draw as point sprites (the CPU ships ~128KB of
  packed floats instead of an 8MB full-frame raster). Gradients are
  evaluated in shaders, so animated gradients stop paying a per-frame
  raster + upload. Video layers upload Y/U/V planes (3.1MB/frame at 1080p
  vs 8.3MB RGBA) and convert BT.601 in the composite pass. Every layer
  type has a GPU path except rain streaks (documented CPU fallback);
  software rasterizers keep the CPU paths where they are genuinely faster
  (the engine measures and chooses at init). Driven by
  `dlopen("libEGL.so.1")` + `eglGetProcAddress`; on X11 it needs nothing
  but the raw window ID. When float render targets are unavailable the
  sims gracefully stay on the CPU solvers (the layer/composite pipeline
  is unaffected).
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
  libwayland-client linkage. GPU-accelerated presentation engages by
  default through the WSI shim (it dlopens libwayland-client at runtime);
  `REV_WAYLAND_GPU=0` opts out for the pure hand-written client. If the
  shim cannot come up — no libwayland-client, or a compositor without
  layer-shell — the engine falls back to the raw client automatically.

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
| Scene distribution  | zip / tar           | **own .revpack container (CRC-checked, path-validated)** |
| Video (optional)    | linked FFmpeg / GStreamer | **dlopen'd libav* — absent = no video, nothing else changes** |
| Threads             | pthreads            | **one optional decode thread (video); everything else lock-free** |

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
Video wallpapers are optional and auto-detected: with FFmpeg development
headers installed (or the vendored `contrib/ffmpeg-headers`), `video.c`
builds and dlopens the system libraries at runtime; without them a stub
builds and the engine is exactly the same minus video (see
[docs/VIDEO.md](docs/VIDEO.md)).

## Usage

```sh
reverie [options] <scene.rev | theme dir | theme.revpack>

  -R, --renderer R    force renderer: auto | vulkan | opengl | cpu
  -B, --backend B     force backend: auto | x11 | wayland
  -f, --fps N         override the scene's target fps
  -m, --monitor N     render only on monitor N
  -o, --once          render one frame to PNG and exit (no display needed)
  -l, --list          list monitors and exit

reverie themes/ripple-lake            # folder theme (hot-reloads)
reverie ripple-lake.revpack           # packed theme — no extraction

Scene layers: image, gradient, solid, fluid, ripple, particles, video —
with blend modes, blur, opacity, parallax, monitor selection, and
keyframe animations. Video layers: `type = video; src = "loop.mp4";
loop = true; paused = false; fit = fill`.

revtool validate <scene.rev | theme>  # check a scene or theme
revtool render  <scene.rev> ...       # offline PNG rendering (with simulated
                                      #   pointer drags for testing interactions)
revtool pack    <scene.rev> [-o out.revpack]   # bundle scene + assets
revtool unpack  <file.revpack> [-d dir]        # extract it back
revtool theme   list|info|validate|pack|inspect  # theme toolkit

See docs/SCENE-FORMAT.md (the scene language), docs/THEMES.md (theme
folders + packed themes), and docs/VIDEO.md (video architecture).
```

## Status — 0.2.0-dev (development, unstable)

No release yet: active development, everything unstable, not ready for
public use — the tree stays a -dev version until it earns a number.
Still no tags and no GitHub releases. This dev cycle:

* **Renderer selection repaired (2026-10).** Auto no longer probes the
  present-only Vulkan tier ahead of the actually-accelerated GL tier (on
  the reporter's NVIDIA machine, plain `reverie <scene>` used to pick
  Vulkan and composite on the CPU at ~6 fps until `--renderer opengl`
  was forced by hand). Explicit requests never fall back silently;
  hardware vs software is detected and reported per device (Vulkan
  physical-device properties: name, type, driver — discrete preferred
  over lavapipe), and software GL/Vulkan initialize only when explicitly
  requested (or opted into for auto) with measured justification in
  docs/BACKENDS.md.
* **The ripple-lake click bug fixed (2026-10).** Clicks on the GPU ripple
  solver launched at ~1/5 strength and never propagated — the shader's
  Laplacian mixed injected and un-injected heights, cancelling the wave
  launch. Now a splat pass injects before the wave passes (matching the
  CPU solver's order); clicks produce expanding rings centered on the
  pointer with e2e click-placement tests pinning it. Same investigation
  fixed a same-frame double-click collapsing onto one position, a
  black-screen path when a ripple scene fell back to CPU compositing on
  the GL tier, and honest GPU timer-query detection (a functional probe
  must pass before "available" is logged).

* **GPU-first rendering, everywhere it belongs.** The maintainer's
  direction after the 1050 Ti field report (fluid-aurora at 57-59 fps —
  the GPU tier finally faster than the CPU tier): if the GPU is there,
  it does the per-pixel work. Every layer type now has a GPU path on
  the OpenGL tier: fluids (RGBA16F solver, since the last cycle),
  ripples (wave equation + refraction composite in shaders — ripple
  scenes used to be locked out of GPU compositing entirely and ran the
  identical CPU pipeline at 27 vs 28 fps), particles (point sprites:
  ~128KB of packed floats instead of an 8MB CPU raster + upload),
  gradients (shader evaluation; animated gradients stop paying a
  per-frame raster + upload), video (Y/U/V plane upload + BT.601
  conversion in the composite pass). Software rasterizers keep the CPU
  path where it genuinely wins (llvmpipe wide points crawl at 6 fps vs
  60 — measured, and the engine chooses accordingly at init). The CPU
  renderer remains the untouched reference implementation.
* **An honest profiler.** `REV_PROFILE=1` reports frame-time percentiles
  (p50/p95/max — averages hid multi-hundred-ms stalls), splits render
  wall time from its upload subset, and shows GPU time from async
  driver timer queries, labeled as such — "gpu timing unavailable" when
  the driver has none, never a made-up number.
* **Themes.** Scenes ship as self-contained folders (theme.json +
  scene + assets) or single-file packed `.revpack` v2 themes, loaded
  without extraction, byte-identical either way (e2e-verified). Entry
  names are path-validated (traversal/absolute/control chars rejected);
  executable content is refused outright. `revtool theme
  list/info/validate/pack/inspect`.
* **Video wallpapers.** MP4 loops decode through dlopen'd FFmpeg
  (optional at build AND run — absent libraries are a non-event), with
  a bounded frame queue on the engine's only extra thread, PTS-driven
  playback on a pause-aware delta-time playhead, looping via seek, and
  GPU color conversion. docs/VIDEO.md records the full decoder
  architecture assessment (why a from-scratch H.264 decoder is the
  wrong project, licensing notes, and the phase-2 hardware-decode plan).
* **Verification.** 8 unit suites (new: theme, video), e2e 11/11 under
  Xvfb (new legs: GPU ripple, folder/packed theme parity, corrupt-pack
  rejection, video on CPU and GL tiers), ASan+UBSan clean. Field
  validation on the 1050 Ti box remains open: the GPU-path performance
  claims come from paired measurements on this 2-core sandbox plus the
  user's prior report; the profiler now gives the next field run the
  numbers to confirm them.

## Status — 0.1.6 (pre-release)

0.1.6 fixes the field-reported 0.1.5 startup failure — on strict compositors
both Wayland paths died at window creation with
`wl_display#1: error 1: invalid arguments for zwlr_layer_surface_v1#N.set_size`:

* **The hand-written `zwlr_layer_surface_v1` opcode table was wrong** — it
  believed the protocol has no `set_size` request and dropped it, shifting
  the whole family by one. Reverie sent its anchor value 15 as a 4-byte
  payload on opcode 0, which is really `set_size(width, height)` (8 payload
  bytes); compositors that validate request framing strictly (KWin et al.)
  post `invalid arguments ... set_size` and kill the connection — while the
  lenient weston-based E2E never reached layer-shell at all (weston does not
  implement the protocol). The client now sends the canonical wallpaper
  sequence from the real `wlr-layer-shell-unstable-v1.xml`: `set_size(0,0)`
  (zero = let the anchors decide), `set_anchor(all edges)`,
  `set_exclusive_zone(-1)`, `set_keyboard_interactivity(none)`, and — the
  request that would have failed next — `ack_configure` moved to its real
  opcode 6 (it was being sent as `get_popup`). The same corrections went
  into the WSI shim's `wl_message` metadata, so libwayland now marshals
  real frames. A latent raw-client bug went with it: the pre-v4
  `wl_surface.damage` fallback sent a 20-byte payload where the protocol
  defines 16 (and buffer coords where logical coords belong).
* **Why four green releases shipped it**: the fake-compositor tests and the
  client were written together off the same wrong table — both sides of the
  socket agreed with each other and with nothing else. The fake servers now
  validate every request against the canonical XML (real opcodes, real
  per-request wire sizes, values pinned by name), and the A/B proof is in
  the tree: the 0.1.5 client fails the corrected suite with the exact field
  symptom ("layer surface did not configure") while the fixed one passes
  7/7.

0.1.5 fixes the first field-reported 0.1.4 crash — `REV_WAYLAND_GPU=1`
segfaulted at startup on real compositors (reported on KWin + NVIDIA):

* **The `wl_output` listener table stopped at 4 slots while outputs are
  bound at version 4** — and v4 compositors (KWin, weston >= 10) send
  `wl_output.name` and `wl_output.description` (opcodes 4 and 5) right
  after the bind. libwayland indexes `implementation[opcode]` with **no
  bounds check**, so the handler pointer was read past the array into
  unrelated data and called: silent segfault inside `wlshim_platform_init`,
  before the first backend log line. The 0.1.4 fake-compositor test never
  sent those events (the coverage hole); the weston E2E ran the *raw*
  client only. Both are fixed: the fake server now delivers the v4 event
  burst, the listener covers all six events, and `e2e-wayland.sh` part C
  drives the shim against live weston (verified: the old table aborts
  there, the new one dispatches cleanly).
* **The shim rejected the default present path** — `wlshim_present_cpu`
  logged "CPU present requested in GPU mode — skipped" and returned false,
  so the default `REV_VK_PRESENT=platform` vulkan tier rendered into
  nothing (a black desktop waiting behind the segfault). The shim now
  presents through `wl_shm` itself — memfd pool via `SCM_RIGHTS`,
  two ARGB8888 buffers cycling on `wl_buffer.release`, premultiplied,
  `set_buffer_scale`/`damage_buffer` marshalled through libwayland —
  mirroring the raw client's field-proven presenter. The regression test
  mmaps the pool server-side and checks the pixels.

0.1.4 makes the GPU-on-Wayland path (`REV_WAYLAND_GPU=1`, the libwayland WSI
shim for EGL/Vulkan) actually work — it had been dead on arrival since 0.1.0,
silently falling back to the raw client, because five independent bug
families only surface when a *real* libwayland marshals our hand-written
interface metadata:

* **`wl_message` was declared with two fields instead of three** (`name`
  first, then `signature`, then `types`) — every metadata table was shifted
  a slot; libwayland read the signature as the name and dereferenced past
  the struct for the types array.
* **`wl_proxy_marshal_flags` was called wrong** — without the `NULL`
  placeholder the new_id vararg slot consumes (every later argument shifted),
  with flags=1 (`WL_MARSHAL_FLAG_DESTROY` — destroys the shell proxy) instead
  of 0, and with version 0 instead of the shell's bound version.
* **Inline-only wrapper functions were dlsym'd as if they were symbols of
  the shared object** — `wl_display_get_registry`, `wl_registry_bind`,
  `wl_registry_add_listener`, `wl_compositor_create_surface`,
  `wl_surface_commit`, and `wl_seat_get_pointer` exist only inside
  `wayland-client-protocol.h`; dlsym returns NULL for all of them, so shim
  init failed at the symbol gate and *always* fell back. These calls are now
  marshalled directly via the exported `wl_proxy_marshal_flags` /
  `wl_proxy_marshal_constructor` with the library's exported
  `wl_*_interface` data symbols.
* **NULL listener slots aborted the process** — real compositors routinely
  send `wl_output.done`, `wl_seat.name`, and `wl_pointer.leave`/axis events;
  a NULL slot in the listener array makes libwayland call abort() when the
  event arrives. All slots are now populated (no-ops where unused).
* **The layer surface was registered only *after* waiting for configure** —
  `on_layer_configure` searches `S.wins[]` bounded by `S.nwins`, which was
  incremented at the end of `wlshim_add_window`, so every configure event
  missed its window, and the dispatch loop then blocked forever on the next
  read: daemon alive, desktop black.

The new `tests/test_wlshim.c` is the regression net: it drives the shim
end-to-end against a strict fake compositor with a genuine libwayland-client
doing the marshalling (handshakes, binds through our hand-written metadata,
`get_layer_surface` on the wire with layer/background/namespace pinned,
configure/ack, pointer events, closed, zero validator violations, ASan+UBSan
clean). It would have caught every one of the five families.

0.1.3 fixes a field-reported 0.1.2 bug — the Wayland wallpaper went black on
real layer-shell compositors (KWin):

* **The whole `zwlr_layer_surface_v1` request family was encoded with the
  wrong opcodes** — `get_layer_surface` as opcode 1 (which is *destroy*),
  `ack_configure` as opcode 6 (also *destroy*), a `set_size` request that
  does not exist in the protocol, and layer value 1 (bottom) where 0
  (background) was meant. KWin answered "invalid object 12", the layer
  surface never configured, and no window was ever created. The
  fake-compositor test had agreed with the wrong numbering (client and test
  were written together), so it only surfaced against a real compositor.
  0.1.3 sends the correct opcodes, sizes buffers from the compositor's
  `configure` event, makes the fake compositor *strict* (object/opcode/size
  validation with real `wl_display.error` responses), fixes the documented
  `--shot FILE --shot-after N` CLI form, and exits with an error instead of
  running windowless when window creation fails.

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
