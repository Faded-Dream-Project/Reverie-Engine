# Reverie Engine — backends & renderer tiers

This document explains how Reverie talks to display servers, how it picks a
renderer, and what each tier does. Everything here is hand-written C11; the
only linked libraries are libc and libm.

## The backend matrix

|                        | X11                          | Wayland                                  |
|------------------------|------------------------------|------------------------------------------|
| wire protocol          | hand-written, raw socket     | hand-written, raw socket                 |
| linked libraries       | none                         | none                                     |
| wallpaper placement    | EWMH `_NET_WM_WINDOW_TYPE_DESKTOP` + `_NET_WM_STATE_BELOW/STICKY` | `zwlr_layer_shell_v1` background layer |
| presentation (CPU tier)| MIT-SHM double buffer (core PutImage fallback) | wl_shm pools (memfd), 2 buffers, premultiplied ARGB8888 |
| monitors               | Xinerama (fallback: root)   | wl_output geometry/mode/scale            |
| hidpi                  | not applicable (core X11)   | scale factor honored (pixel-sized buffers + `set_buffer_scale`) |
| input                  | QueryPointer polling + latched button edges | wl_seat/wl_pointer events (consume-on-read, click latching) |
| frame pacing           | X connection fd polling | wl_surface.frame callbacks via wait_frame() |
| GPU present            | EGL (raw XID) or Vulkan (runtime-dlopen'd libxcb) | experimental WSI shim (`REV_WAYLAND_GPU=1`) — EGL via wl_egl_window |

### Selection

`--backend` / `REV_BACKEND` accept `auto | x11 | wayland`. Auto order:

1. `WAYLAND_DISPLAY` set → wayland
2. `DISPLAY` set → x11
3. a `wayland-0` socket exists in `XDG_RUNTIME_DIR` → wayland
4. else x11

An explicit backend never silently falls back. Auto does.

### The Wayland big caveat

Placing a wallpaper on Wayland requires the compositor to support the
`zwlr_layer_shell_v1` protocol. wlroots-based compositors (sway, Hyprland,
Wayfire, river, labwc), KWin and others do. **GNOME's mutter does not** —
GNOME treats the desktop background as compositor state, so third-party
wallpaper engines cannot exist there without compositor extensions. When
the global is missing, Reverie exits with an explicit message saying
exactly that.

## The renderer chain

`--renderer` / `REV_RENDERER` accept `auto | vulkan | opengl | cpu`. Auto
order: **vulkan → opengl → cpu**, each fully initialized in turn; the first
success wins and every failure is logged with its reason. (llvmpipe is not
a separate probe — it *is* the opengl tier, detected by GL_RENDERER string
and logged as such.)

### tier 3 — vulkan (present path)

* Everything loaded at runtime: `dlopen("libvulkan.so.1")`, entry points via
  `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`.
* The structure types, enums, and struct layouts are vendored by hand and
  verified against the official Khronos headers.
* X11 WSI: `vkCreateXcbSurfaceKHR` with an xcb connection from a
  runtime-dlopen'd `libxcb.so.1`. The window XID comes from our own raw X11
  client — server-side objects are global, so this is legal without Xlib.
  Wayland WSI: the shim's `wl_display*`/`wl_surface*` (experimental).
* **Present path (0.1.1):** the engine composites on the CPU and then either
  - *platform present (default)* — hands the frame to the backend's proven
    presenter (MIT-SHM on X11, wl_shm on Wayland), or
  - *WSI present (`REV_VK_PRESENT=wsi`)* — memcpy into a host-visible
    staging buffer → `vkCmdCopyBufferToImage` with layout barriers → FIFO
    present.
  The default flipped in 0.1.1 after a field report of a **black screen on
  XWayland with a real GPU driver** (see Troubleshooting); both paths are
  E2E-tested and pixel-exact vs the CPU renderer on lavapipe.

### tier 2 — opengl (GPU compositing)

* Everything loaded at runtime: `dlopen("libEGL.so.1")`, GL entry points via
  `eglGetProcAddress`. On X11, `eglGetDisplay(EGL_DEFAULT_DISPLAY)` makes
  Mesa open its own connection — the engine hands EGL only the raw XID, so
  **no Xlib exists anywhere in the process** (not even dlopen'd).
* On Wayland (WSI shim active) the display is opened via
  `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR)` and every window
  surface lives on a `wl_egl_window` from `libwayland-egl.so.1` (also
  dlopen'd) — the raw `wl_surface` is not an EGL native window (0x300b,
  see Troubleshooting). Buffer-pixel dimensions throughout; output
  geometry changes resize the wrapper in place. While EGL owns a window,
  the shim's `wl_shm` presenter is locked off its surface.
* Pipeline: upload straight-alpha raster → premultiply pass → downsample +
  separable 9-tap gaussian + upscale → W3C blend shader onto a ping-pong
  accumulation FBO → present (unpremultiply).
* The blend shader mirrors `composite.c`'s `blend_pixel()` exactly, so GPU
  and CPU output stay comparable; the blur is a gaussian approximation
  where the CPU tier uses a 3-pass box filter — small visual differences
  under large blur radii are expected and documented.
* Ripple scenes composite on the CPU (the ripple filter needs the
  below-content as feedback); every other layer type runs on the GPU.
* Verified E2E on llvmpipe: structurally identical output to the CPU tier.

### tier 1 — cpu (the floor)

* The engine's own compositor: W3C separable blends, premultiplied 3-pass
  box blur, bilinear resampling, analytic-AA rasters.
* Presentation: MIT-SHM on X11, wl_shm on Wayland.
* Always available — if every GPU probe fails, you still get a wallpaper.

## Environment variables

| variable            | effect                                              |
|---------------------|-----------------------------------------------------|
| `REV_RENDERER`      | same as `--renderer`                                |
| `REV_BACKEND`       | same as `--backend`                                 |
| `REV_VK_PRESENT`    | `wsi` = vulkan tier presents via its swapchain (default: platform SHM path; on Wayland+shim that is the shim's own wl_shm presenter) |
| `REV_WAYLAND_GPU`   | `1` = activate the experimental Wayland WSI shim    |
| `REV_WL_LIB`        | explicit `libwayland-client.so` path for the shim (tests pin a known library; default: system soname) |
| `REV_WL_TRACE`      | `1` = dump raw Wayland wire traffic to stderr (diagnostics) |
| `REV_PROFILE`       | `1` = append a per-frame stage breakdown (`sim / raster / render+present` ms) to the periodic `perf:` line |

## Troubleshooting

* **GPU renderer active but the scene is slow / one CPU core is pinned
  (dev-cycle postmortem)** — field report: `--renderer opengl` on a GTX
  1050 Ti showed the real NVIDIA renderer string (GL was finally doing
  the compositing) yet the wallpaper ran at ~4 fps with a CPU fan
  spinning. This is not the GPU path failing — the GPU tier composites
  and presents, but the *scene work* (fluid simulation, animated layer
  rasters) runs on the CPU, single-threaded, by design. The dev cycle
  after that report removed the avoidable CPU costs: the fluid layer's
  full-res bilinear upscale + `exp()` tonemap (now a LUT, and in GPU
  compositing mode the layer is emitted at dye resolution and stretched
  with linear texture sampling on the GPU), the f64 pressure solve (now
  f32 — bit-identical for the power-of-two divisors it used), and the
  animated-gradient polygon re-rasterization (now a direct rect fill,
  which also fixed a smear bug where transparent gradient regions froze
  at old content). What remains on the CPU is the simulation itself: the
  engine has no threads, so a fluid scene will always keep one core
  busy — less busy now, but busy. Run with `REV_PROFILE=1` and read the
  `perf:` stage line before reporting: if `sim` dominates, that is the
  single-threaded solver; if `render+present` dominates on a *hardware*
  GPU, look at the driver path instead. Truly idle-CPU simulation means
  GPU compute, which is future work.
* **`eglCreateWindowSurface failed (0x300b)` with `--renderer opengl` on
  Wayland (dev-cycle postmortem)** — field report: with `REV_WAYLAND_GPU=1`
  active, the GL tier died at "renderer init" with EGL error 0x300b
  (`EGL_BAD_NATIVE_WINDOW`). Root cause: the renderer passed the shim's
  raw `wl_surface*` as the EGL native window. On X11 the XID *is* the
  native window, and the X11 path worked — but on Wayland EGL requires a
  `wl_egl_window` wrapper (created per window by `libwayland-egl.so.1`,
  dlopen'd with no build-time linkage) and rejects anything else. The
  fixed sequence: `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,`
  `wl_display, NULL)` when the client extensions advertise
  `EGL_EXT_platform_wayland` (plain `eglGetDisplay` fallback),
  `wl_egl_window_create(wl_surface, w, h)` with BUFFER-pixel dimensions
  (the engine's framebuffers are already device-pixel sized),
  `eglCreatePlatformWindowSurface` on the wrapper, `wl_egl_window_resize`
  on output geometry changes (the EGL surface itself survives), and
  `wl_egl_window_destroy` at teardown. While a window is EGL-owned the
  shim's `wl_shm` presenter is locked off that surface
  (`wlshim_set_gl_surface`) — two present paths attaching buffers to the
  same `wl_surface` would corrupt each other's commits.
  `scripts/egl_wayland_probe.c` (run via `scripts/run-egl-probe.sh`)
  reproduces both paths against a live weston: path A (raw surface)
  fails — the exact EGL error code varies by driver/version; the field
  report was 0x300b — and path B (wrapper) reaches 5 successful
  `eglSwapBuffers`. If EGL still fails in the dev tree, check that
  `libwayland-egl.so.1` is installed alongside the driver's `libEGL.so.1`
  (Mesa ships both; hybrid NVIDIA setups must point EGL at a vendor that
  supports the Wayland platform).
* **`invalid arguments for zwlr_layer_surface_v1#N.set_size` at startup
  (0.1.6 postmortem)** — field report on a strict compositor: BOTH Wayland
  paths (`REV_WAYLAND_GPU=1` and the raw client behind `--renderer
  opengl`) died at "monitor 0: window creation failed" with a
  `wl_display` error naming `set_size`, a request reverie's opcode table
  claimed did not exist. Root cause: the hand-written
  `zwlr_layer_surface_v1` request table had dropped `set_size` — request
  0 in the real `wlr-layer-shell-unstable-v1.xml` — shifting the whole
  family by one. The anchor value 15 was sent as a 4-byte payload on
  opcode 0; strict compositors demarshal that as `set_size(uu)` (needs 8
  payload bytes), post error 1 on the display, and drop the connection.
  Behind it waited two more latent kills: `set_keyboard_interactivity`
  was going out as `set_margin(iiii)` (4 of 16 payload bytes) and
  `ack_configure` as `get_popup`. Why the tests never caught it: the
  fake compositor validated against the same wrong table (client and
  validator were written together), and weston — the only *real*
  compositor in the E2E — does not implement layer-shell, so no live
  test ever reached the request burst. Fixed by rebuilding the client,
  the shim's `wl_message` metadata, and both fake servers from the
  canonical XML: `set_size(0,0)+anchor(all)+zone(-1)+interactivity(none)`
  in real opcodes, `ack_configure` at opcode 6, per-request wire sizes
  enforced, and an A/B proof that the 0.1.5 client fails the corrected
  suite with the exact field symptom while the fixed one passes. Bonus
  fix: the pre-v4 `damage` fallback now sends the protocol's 16-byte
  payload with logical coordinates. If you still see a compositor error
  after 0.1.6, run with `REV_WL_TRACE=1` and check the reported
  request name against the XML tables in `tests/test_wayland.c`.
* **`REV_WAYLAND_GPU=1` segfaults at startup / listener tables (0.1.5
  postmortem)** — field report on KWin: the daemon died with a silent
  SIGSEGV right after loading the scene, before any backend log line.
  Root cause: `wl_output` was bound at **version 4** but its listener
  table stopped at 4 slots (`geometry`, `mode`, `done`, `scale`) — and v4
  compositors (KWin, weston >= 10) send `wl_output.name` and
  `wl_output.description` (opcodes 4/5) immediately after the bind.
  libwayland indexes `implementation[opcode]` with **no bounds check**:
  the handler pointer is read past the array into whatever sits there and
  called — depending on the binary layout that is a wild jump (segfault,
  the field report) or a NULL (libwayland aborts with `listener function
  for opcode N of wl_output is NULL`, the local weston repro). The 0.1.4
  regression net missed it because the fake compositor never sent v4-only
  events and the weston E2E ran the raw client only. Fixed by covering
  every event of the bound version (six slots for wl_output, wl_shm
  format, wl_buffer release), dispatching roundtrip #2 *before* the
  layer-shell gate so even shell-less compositors exercise the event
  path, teaching the fake server the v4 burst, and adding
  `e2e-wayland.sh` part C (shim against live weston — the old table
  aborts there, verified A/B).
* **GPU mode presented nothing (0.1.5)** — with the shim active the
  default `REV_VK_PRESENT=platform` path called `wlshim_present_cpu`,
  which logged "CPU present requested in GPU mode" and returned false —
  every frame was rendered and dropped. The shim now runs its own
  `wl_shm` presenter (memfd pool via `SCM_RIGHTS`, two ARGB8888 buffers
  cycling on `wl_buffer.release`, premultiplied uploads,
  `set_buffer_scale` + `damage_buffer` through libwayland), mirroring
  the raw client's presenter; the fake-compositor test mmaps the pool
  and checks pixels + cycling. `REV_VK_PRESENT=wsi` keeps the true
  swapchain path (driver-owned buffers on the shim's wl_surface).
* **"wlshim: missing libwayland symbols" / shim never activates (0.1.4
  postmortem)** — the GPU-on-Wayland path (`REV_WAYLAND_GPU=1`) was dead on
  arrival from 0.1.0 through 0.1.3: init failed at the symbol gate and the
  platform silently fell back to the raw client, so nobody noticed. Five bug
  families, all invisible until a *real* libwayland actually marshals the
  shim's hand-written metadata:
  (1) `wl_message` declared two-field instead of three-field (`name`
  first) — every metadata table shifted by a slot;
  (2) `wl_proxy_marshal_flags` called without the NULL placeholder the
  new_id vararg consumes, with flags=1 (DESTROY!) instead of 0, and
  version 0 instead of the shell's bound version;
  (3) dlsym of inline-only protocol wrappers (`wl_display_get_registry`,
  `wl_registry_bind`, `wl_compositor_create_surface`, `wl_surface_commit`,
  `wl_seat_get_pointer`, ...) which are NOT symbols of the shared object —
  dlsym returns NULL for every one; all such calls are now marshalled
  directly via the exported `wl_proxy_marshal_flags` /
  `wl_proxy_marshal_constructor` entry points with the exported
  `wl_*_interface` data symbols;
  (4) NULL listener slots (`wl_output.done`, `wl_seat.name`,
  `wl_pointer.leave`/axis family) — libwayland aborts the process when an
  event lands on a NULL slot, and real compositors send those routinely;
  (5) the layer surface was registered in `S.wins[]` only *after* waiting
  for configure — the configure listener searches by `S.nwins`, so the
  event missed, and the dispatch loop blocked forever on the next read
  (daemon alive, desktop black). Fixed in 0.1.4; the regression net is
  `tests/test_wlshim.c`: the shim runs end-to-end against the strict fake
  compositor with a genuine libwayland-client doing the marshalling —
  binds prove our interface metadata, `get_layer_surface` proves the
  wl_message tables and the marshal ABI, and the strict validator sees
  zero violations (ASan+UBSan clean, proxies destroyed on fini).
* **"compositor lacks ... zwlr_layer_shell_v1"** — GNOME Wayland, or a
  compositor without layer-shell (stock weston is another). See the caveat
  above; use X11 (XWayland counts as X11) or a wlroots/KDE compositor.
  Since 0.1.2 the message also reports how many globals were parsed, so a
  false positive of this kind is immediately visible (see the postmortem
  below).
* **"compositor lacks ... zwlr_layer_shell_v1" on a compositor that HAS
  layer-shell (0.1.2 postmortem)** — field report: a stacking compositor
  advertised 57 globals (layer shell at #52, `wl_output` at #57), yet the
  engine claimed layer shell was missing and fell back to XWayland. Root
  cause: the registry's global table had a fixed capacity of **48** and
  silently dropped everything past it — the parse was correct (the trace
  printed every global), the *storage* was lossy. The test suite never saw
  it because weston and the fake compositor advertise ~19 globals. Fixed by
  growing the table on demand (same fix in the libwayland WSI shim, which
  had its own 48-entry copy); the message now includes the parsed count;
  a flood regression test replays the exact field layout (57 globals,
  shell at #52, output at #57) and fails on the old code.
* **Black screen on a Wayland session (0.1.0 postmortem)** — the classic
  0.1.0 failure chain on such sessions: the Wayland client failed its
  handshake (bug below), the engine auto-fell back to X11 — which under a
  Wayland session means **XWayland** (detected and logged as such since
  0.1.1) — and the vulkan tier presented through its WSI swapchain, which
  some real-driver XWayland stacks silently drop: every call succeeds, the
  window never updates. 0.1.1 fixes both links: the Wayland handshake now
  works natively (so the fallback usually never happens), and the vulkan
  tier defaults to the platform presenter that the pre-0.1.0 engine shipped
  (MIT-SHM into the same desktop window — the mechanism field-proven to
  show). `REV_VK_PRESENT=wsi` restores the swapchain path if you want to
  benchmark it. If the fallback still shows black on your stack, bisect it:
  run `reverie <scene> --renderer cpu` (if the CPU tier shows, present and
  compositing are fine and the delta is the GPU init), and use
  `--shot out.png --shot-after 3` to read the window content back — a
  correct PNG under a black screen means the window is being obscured or
  restacked by the WM, not mis-rendered. (`--shot FILE --shot-after N` and
  the legacy `--shot N FILE` forms are both accepted.)
* **"compositor error … code 0: invalid object N" / "layer surface did not
  configure" (0.1.2 postmortem)** — the raw client encoded the whole
  `zwlr_layer_surface_v1` request family with opcodes shifted off the
  protocol's numbering: `get_layer_surface` was sent as opcode 1 (which is
  *destroy*), `ack_configure` as opcode 6 (also *destroy*), plus a
  `set_size` request that does not exist in the protocol at all. The first
  bad request made the compositor kill the connection — KWin reported
  "invalid object 12" (the never-created layer surface) — so the surface
  never configured, no window was created, and the desktop stayed black
  while the daemon ran on. The fake-compositor test had agreed with the
  wrong numbering because client and test were written together (same class
  of bug as the 0.1.0 framing swap). Fixed in 0.1.3: correct opcodes, layer
  enum BACKGROUND=0 (was sending 1 = bottom), no set_size (the size comes
  from `configure`), and the fake compositor is now a *strict* one — it
  tracks every object id, validates opcode and wire size of every request,
  and answers violations with a real `wl_display.error` exactly like KWin.
  The daemon also exits with an error when no window could be created
  instead of running windowless.
* **"protocol parse error" / "registry roundtrip timed out" (0.1.0
  postmortem)** — the wire client *encoded* requests with the correct header
  layout (`word1 = (size << 16) | opcode`) but *decoded* events with the two
  16-bit fields swapped, so the first real compositor message failed
  framing; the sync callback then never fired and the roundtrip timed out.
  The fake-compositor test had missed it because both sides of that test
  shared the same swapped decoder. Fixed in 0.1.1; the regression net is now
  triple: the fake compositor uses the real format, a golden-byte test pins
  our first 24 wire bytes to a capture from real libwayland, and
  `scripts/e2e-wayland.sh` runs the client against a genuine weston.
* **"vulkan unavailable (...)"** — read the reason in the log: no loader,
  no ICD, no presentable queue family, or the surface does not allow
  TRANSFER_DST usage (the engine falls through to GL).
* **"opengl unavailable (...)"** — usually no libEGL or no EGL vendor; on
  headless test setups set `__EGL_VENDOR_LIBRARY_FILENAMES` if you keep
  Mesa outside the default paths.
* **llvmpipe note in the log** — expected on software GL; it is a fully
  supported tier, not an error.
