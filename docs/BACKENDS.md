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
| GPU present            | EGL (raw XID) or Vulkan (runtime-dlopen'd libxcb) | WSI shim (default on, `REV_WAYLAND_GPU=0` opts out) — EGL via wl_egl_window |

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

## The renderer policy (2026-10 redesign)

`--renderer` / `REV_RENDERER` accept `auto | vulkan | opengl | cpu`.

**Explicit requests are honored exactly and never fall back silently.**
`--renderer vulkan` initializes Vulkan and only Vulkan; if that fails the
daemon EXITS with the init error (the log names the failing call). Software
devices are not substituted silently either — when an explicit request lands
on software (lavapipe Vulkan, llvmpipe GL), it RUNS, and the log says
`SOFTWARE` in every renderer line plus a dedicated warning. "Initialized" is
never confused with "hardware accelerated": every tier reports its device
name, device type, and hardware/software classification at init.

**Automatic selection is deterministic** (render.c, documented + measured):

1. **OpenGL with a hardware context** — the only tier that actually renders
   and simulates on the GPU today, so it is the first and only GPU probe.
2. **The native CPU renderer** when GL initializes as software — llvmpipe
   would boot an entire software GL stack to do what the CPU renderer does
   natively (and on llvmpipe the GL tier itself keeps sims, point sprites
   and gradients on the CPU for speed). Override with `REV_AUTO_GL_SW=1`.
3. **The Vulkan tier (hardware devices only)** — the GPU compute
   compositing tier (see below): every layer op and the fluid/ripple
   solvers run as compute kernels. A software Vulkan device (lavapipe)
   boots a whole software stack to do what the CPU renderer does natively,
   so software Vulkan stays out of auto. Override with `REV_AUTO_VK=1`.

### why auto does not probe software stacks — the measurements

llvmpipe-GL vs the native CPU renderer, 1080p, this repository's E2E stack,
best-of-two 5 s windows (interactive scenes driven by `REV_FAKE_POINTER`):

| scene               | native CPU | llvmpipe GL | verdict              |
|---------------------|-----------:|------------:|----------------------|
| ripple-lake (drag)  |     53.3   |       51.4  | CPU, ~4%             |
| snowfall            |      4.7   |        5.8  | llvmpipe, ~23%       |
| ember-drift (drag)  |     22.4   |       14.5  | CPU, ~54%            |
| fluid-aurora (drag) |      6.8   |        7.7  | llvmpipe, ~13%       |
| animated-gradients  |     14.8   |       14.1  | parity               |

Neither is uniformly faster — which is exactly why the *default* picks the
zero-dependency, consistent native CPU path and the *override* is one env
var away. The numbers above are from a 2-core sandbox; both paths are
irrelevant on machines with working hardware GL, which is what auto actually
selects.

### tier 3 — vulkan (GPU compute compositing)

* Everything loaded at runtime: `dlopen("libvulkan.so.1")`, entry points via
  `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`.
* The structure types, enums, and struct layouts are vendored by hand and
  verified against the official Khronos headers. The validation layer runs
  clean on the whole frame path (`REV_VK_VALIDATE=1` with
  `glslang`-validated SPIR-V — see scripts/gen_spv.sh).
* X11 WSI: `vkCreateXcbSurfaceKHR` with an xcb connection from a
  runtime-dlopen'd `libxcb.so.1`. The window XID comes from our own raw X11
  client — server-side objects are global, so this is legal without Xlib.
  Wayland WSI: the shim's `wl_display*`/`wl_surface*`.
* **Device selection (2026-10):** every physical device is enumerated and
  reported (name + type); the tier picks by hardware class —
  discrete > integrated > virtual > other > CPU — among devices with a
  GRAPHICS+COMPUTE queue family that can present with a TRANSFER_DST
  swapchain. Loader enumeration order no longer decides anything (with a
  software ICD listed first, the old first-match loop used to pick lavapipe
  while a real GPU sat behind it). The selected device, its type, Vulkan
  version, driver version, and the honest hardware/software verdict are
  logged.
* **GPU compute compositing (0.2.0 — the fix for the third field report):**
  the tier used to be PRESENT-ONLY (the engine composited on the CPU and
  uploaded finished frames; the reporter measured sim climbing to 37.5 ms
  on ripple-lake). Now every layer operation runs as a SPIR-V compute
  kernel (src/shaders/*.comp, embedded in src/vk_spv.h, regenerated by
  scripts/gen_spv.sh — committed, so builds need no shader compiler):
  - the W3C blend composite (all 5 blend modes, placement, opacity,
    straight/premultiplied sources),
  - the separable blur chain (premultiply → downsample → H/V 9-tap →
    upscale),
  - gradient evaluation (shader-evaluated, cached by content serial),
  - particle layers — tile-binned *ordered* source-over splats (one 8x8
    workgroup owns a 64x64 tile; a shared-memory list preserves blend
    order, so the result is the GL point-sprite path's exact per-pixel
    sequence, not an approximation),
  - video planes + BT.601 conversion + blend in one dispatch,
  - the full fluid solver (splats, vorticity, Jacobi projection,
    advection, tonemap) and the ripple wave equation + refraction
    composite on RGBA16F storage images — identical constants and pass
    order to the GL tier's shaders.
  The engine's CPU compositor is never in the frame path; the CPU records
  dispatches, and (on the platform present path) copies the finished frame
  out. E2E-verified **bit-exact against the CPU renderer** on both present
  paths (static scenes compare byte-for-byte; ripple click parity holds).
* **GPU timing:** core Vulkan 1.0 timestamp queries bracket every frame's
  compute; `REV_PROFILE` reports them asynchronously (several frames
  behind) exactly like the GL tier's timer queries — labeled `gpu=...*`,
  never conflated with CPU wall time. The query pairs cycle on the frame
  slot rotation, which advances on *every* `end()` outcome — a rotation
  that stalled on failed presents is what made the WSI path report
  `gpu=0.0 "unavailable"` in the fifth field report while init had just
  logged the queries as available.
* **Present paths (mechanism unchanged since 0.1.1, fed by GPU frames —
  staging fixed in 0.2.0 after the fourth field report):**
  - *platform present (default)* — the finished image is copied to a
    host-visible buffer and handed to the backend's proven presenter
    (MIT-SHM on X11, wl_shm on Wayland). Same pixels, same window, the
    mechanism every field report confirmed; the GPU does all the rendering.
    The 0.2.0 fix, in two parts:
    - **The staging buffer is HOST_CACHED memory.** It used to prefer
      DEVICE_LOCAL among host-visible types, which on a discrete GPU means
      a BAR window into VRAM: *uncached* CPU reads, ~500 ns per 4-byte
      load. The wl_shm presenter's per-pixel premultiply read that memory
      pixel-by-pixel — 2.07 M serialized bus reads ≈ **1.1 s per 1080p
      frame** on a GTX 1050 Ti (field report #4: `render=1112 ms`,
      `gpu=2.1 ms`, one core pegged; software devices never see this —
      lavapipe's single memory type is cached sysmem, which is why E2E
      missed it). Selection now tiers: HOST_VISIBLE|HOST_CACHED|COHERENT →
      HOST_CACHED (+`vkInvalidateMappedMemoryRanges` after the fence when
      non-coherent) → HOST_VISIBLE|COHERENT *preferring non-DEVICE-LOCAL*
      → any. The chosen class is logged (`frame download staging: memory
      type …`).
    - **The premultiply AND the byte swap run on the GPU.** wl_shm
      ARGB8888 is premultiplied *and* packs its u32 as `0xAARRGGBB` —
      memory bytes B,G,R,A — while the engine's native u32 is
      `0xAABBGGRR` (memory R,G,B,A). The straight→premul conversion is
      one extra `copypremul` compute dispatch into the accum ping-pong
      twin that also exchanges R and B on store (`p[1].y` flag, from
      `RPlatform::present_bgra`), downloaded after it, and the presenter
      receives `premultiplied=true` → a single `memcpy` (~1 ms at 1080p)
      — the CPU never touches a per-pixel loop on the Vulkan tier. X11
      has no premul or byte-order convention (`present_premul=false`,
      `present_bgra=false`): bytes copy through straight and x11.c
      converts into the visual's masks, bit-identical to the CPU tier
      (E2E parity stays byte-exact). The CPU tier's wl_shm leg and the
      raw Wayland client swap R/B in their premultiply loops (~5 ms at
      1080p, cached RAM).
    GPU-side cost of the present download at 1080p: one 8.3 MB DMA + one
    tiny kernel, both inside the timestamped region — reported in `gpu=`.
  - *WSI present (`REV_VK_PRESENT=wsi`)* — the image is blitted into the
    swapchain (BGRA8 UNORM preferred first: the blit maps components by
    name so the R/B swap is free, and BGRA8 is the fourcc every Wayland
    compositor imports natively while RGBA8/ABGR8888 support varies) and
    presented FIFO, zero-copy. Surfaces that only offer _SRGB swapchain
    formats get one sRGB-decode compute pass first so the blit's encode
    roundtrips to the identity.
    The 0.2.0 wedge-free acquire flow (fifth field report: 1.0 fps,
    frozen wallpaper, `frame p50` exactly 1000.0 ms): images are acquired
    in 8 ms slices with the platform event queue pumped between attempts
    (Wayland WSI releases and input flow while waiting), a frame gives up
    after ~250 ms and logs a throttled `swapchain starved` warning, and
    the swapchain is created with `minImageCount + 2` headroom so FIFO
    acquires do not race the compositor's release cycle head-on. The sync
    fence is reset only after an image is actually acquired — the old
    code reset it first and could return without ever submitting, leaving
    every later frame waiting a dead fence for a full second each.
  The default stays platform-SHM because the 0.1.0 WSI path had a black
  screen on one real XWayland stack; both paths are E2E-tested and
  pixel-exact. Device loss (`VK_ERROR_DEVICE_LOST`) at submit or present
  is reported explicitly and stops presenting; OUT_OF_DATE recreates the
  swapchain.
* **Software devices (lavapipe):** the tier initializes and reports
  honestly, but GPU sims / particles / gradients are declined by default
  (a CPU-running Vulkan device executes compute slower than the native CPU
  paths — the same policy the GL tier applies to llvmpipe).
  `REV_VK_SIMS_SW=1` forces them on for regression testing; that is what
  E2E uses to keep the whole GPU sim path tested without hardware.
* **Synchronization:** 3 frame slots per window, each with its own command
  pool, descriptor pool, 512-byte-stride dynamic uniform buffer and upload
  arena; a slot is reused only after its fence retired. Images live in
  `VK_IMAGE_LAYOUT_GENERAL` for their whole lifetime with explicit access
  barriers. Replaced resources go through a graveyard freed three frames
  later, so ping-pong reallocations never pull memory out from under an
  in-flight frame.

### tier 2 — opengl (GPU compositing)

* Everything loaded at runtime: `dlopen("libEGL.so.1")`, GL entry points via
  `eglGetProcAddress`. On X11, `eglGetDisplay(EGL_DEFAULT_DISPLAY)` makes
  Mesa open its own connection — the engine hands EGL only the raw XID, so
  **no Xlib exists anywhere in the process** (not even dlopen'd).
* On Wayland (WSI shim — active by default, `REV_WAYLAND_GPU=0` opts out)
  the display is opened via
  `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR)` and every window
  surface lives on a `wl_egl_window` from `libwayland-egl.so.1` (also
  dlopen'd) — the raw `wl_surface` is not an EGL native window (0x300b,
  see Troubleshooting). Buffer-pixel dimensions throughout; output
  geometry changes resize the wrapper in place. While EGL owns a window,
  the shim's `wl_shm` presenter is locked off its surface.
* Pipeline: content-addressed texture cache (static rasters upload only
  when their content serial changes) → blur path: premultiply pass →
  downsample + separable 9-tap gaussian + upscale → W3C blend shader onto a
  ping-pong accumulation FBO holding STRAIGHT alpha (exactly like the CPU
  framebuffer — no premult/unpremult round-trip on faint pixels) → present.
* GPU fluid simulation: when the hardware can render to RGBA16F targets,
  fluid layers arrive as simulation descriptors instead of pixels — splats,
  vorticity confinement, Jacobi pressure projection (the parallel form of
  the CPU's Gauss-Seidel), semi-Lagrangian advection with dissipation
  folded in, and the dye tonemap all run as fragment shaders over
  ping-pong float textures. `sim=` in the REV_PROFILE line drops to ~0.
  Software GL (llvmpipe) keeps the CPU solver — fragment dispatch overhead
  loses to tight C loops there — and `REV_GL_FLUID_SW=1` forces the GPU
  path on software GL for testing.
* The blend shader mirrors `composite.c`'s `blend_pixel()` exactly, so GPU
  and CPU output stay comparable; the blur is a gaussian approximation
  where the CPU tier uses a 3-pass box filter — small visual differences
  under large blur radii are expected and documented.
* GPU ripple simulation (hardware GL, or llvmpipe with
  `REV_GL_FLUID_SW=1`): the height-field wave equation runs in a shader
  pair — a splat pass writes the pointer drip and click impacts into the
  field first, then pure wave-update passes step it (mirroring the CPU
  solver's exact order; see the ripple-lake postmortem below). The
  refraction composite reads the accumulation target as its below-content
  and offsets samples by the surface-normal gradient, with the same
  specular glint the CPU applies. On software GL without the test hook the
  scene falls back to full CPU compositing — and that fallback path now
  presents correctly (black-screen postmortem below).
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
| `REV_WAYLAND_GPU`   | `0` = opt OUT of the Wayland WSI shim (default: active whenever libwayland-client loads) |
| `REV_WL_LIB`        | explicit `libwayland-client.so` path for the shim (tests pin a known library; default: system soname) |
| `REV_GL_FLUID_SW`   | `1` = run the GPU fluid solver even on software GL (test hook; hardware GL always uses it) |
| `REV_WL_TRACE`      | `1` = dump raw Wayland wire traffic to stderr (diagnostics) |
| `REV_PROFILE`       | `1` = append the stage breakdown to the periodic `perf:` line: `sim / raster / render (incl upload) / gpu` ms + frame-time p50/p95/max. `gpu` comes from async driver timer queries on hardware GL and is labeled unavailable when the driver has none |
| `REV_AUTO_VK`       | `1` = let auto also consider the present-only Vulkan tier (off by default) |
| `REV_AUTO_GL_SW`    | `1` = let auto keep software GL (llvmpipe) instead of the native CPU renderer |
| `REV_FAKE_POINTER`  | TEST/BENCH: `<speed>` = synthesize a pressed pointer on a Lissajous path (circuits/s) so interactive scenes benchmark headlessly. Never set by users |
| `REV_FAKE_CLICK`    | TEST: `"x,y[@t][;x,y@t ...]"` = synthesize clicks at normalized positions, each at its own @t seconds (default 0.3 s + 0.2 s spacing) so click-driven interaction (ripple splashes, click-after-calm) is regression-testable headlessly. Never set by users |
| `REV_GL_FLUID_SW`   | TEST: force the GPU sim/point/gradient paths on software GL for regression coverage |
| `REV_RIPPLE_DUMP`   | TEST: `"call,call,..."` = read the GPU ripple height field back at those step-call indices and log statistics (placement debugging) |

## Troubleshooting

* **WSI present wedged at exactly 1 s/frame + every blue rendered red
  (0.2.0 postmortem, the fifth field report)** — field report:
  `REV_VK_PRESENT=wsi` on the GTX 1050 Ti ran at 1.0–1.6 fps with the
  wallpaper frozen and `frame p50` pinned at exactly 1000.0 ms; separately,
  choosing the Vulkan tier turned the theme's blue/purple into
  red/green-ish ("how does the renderer affect the theme wth?"). Two
  independent root causes, both invisible to the sandbox stack:
  1. **A self-poisoning sync fence.** The WSI `end()` reset its sync
     fence *before* `vkAcquireNextImageKHR`; when the acquire timed out
     (1 s) the frame returned without ever submitting — the fence stayed
     reset-and-unsignaled, and because the failure path also skipped the
     slot rotation, every later frame waited that same dead fence for
     another full second: p50 exactly 1000.0 ms, zero presents, and the
     stalled rotation starved the GPU timer pairs (`gpu=0.0
     "unavailable"` although init had logged them usable). The stall was
     seeded by acquire timeouts in the first place — blocking a second
     inside the driver while nothing drained the Wayland socket means
     buffer releases never reach the WSI. Fix — the acquire now runs in
     8 ms slices with the platform event queue pumped between attempts
     (~250 ms budget, throttled `swapchain starved` warning on give-up),
     the fence is reset only after an image is in hand, `minImageCount+2`
     headroom keeps FIFO acquires off the release cycle's heels, and the
     slot rotation advances on every exit so fences, frame slots and
     timer queries keep cycling.
  2. **An R/B byte-order swap on every wl_shm present.** wl_shm
     ARGB8888 packs its u32 as `0xAARRGGBB` (memory B,G,R,A); the
     engine's native u32 is `0xAABBGGRR` (memory R,G,B,A). The shim's
     premultiply loop and the raw Wayland client wrote engine-order
     bytes into ARGB8888 buffers — every blue swapped to red, purple to
     washed red. X11 never showed this (x11.c always repacked into the
     visual's masks), the GL tier never showed it (EGL owns the surface
     and the swap), and E2E pixel parity could not catch it because the
     reference was the CPU tier — two swapped paths agree with each
     other. The old wlshim unit test even used a *symmetric*
     semi-transparent probe (r == b), which is invariant under the swap.
     Fix — `RPlatform::present_bgra` declares the presenter's byte
     order; the Vulkan tier's `copypremul` kernel swaps R and B on store
     (same dispatch as the GPU premultiply, free), and the CPU legs swap
     inside their existing premultiply loops. The unit probes are now
     asymmetric so a regression fails loudly.

* **Ripple lake: the splash expands, glitches once it reaches the screen
  edges, and later clicks do nothing (2026-10 postmortem, the sequel)** —
  field report: "the ripple effect works but after clicking it expands
  until it reaches the sides of the screen and starts glitching, and
  clicking again after it fully expanded does nothing" (GTX 1050 Ti, GL
  tier; the Vulkan tier showed the same physics plus a climbing sim time,
  13.3 → 36.0 ms/frame, pinning one core). Root cause — the wave update
  itself was ANTI-damped. Both solvers computed
  `nh = 2h + 0.24·lap − prev·(1 − damping·0.05)`; for smooth modes that
  recurrence, `z² − 2z + (1−δ)`, has a pole at `z = 1 + √δ ≈ 1.063`
  (δ = 0.08·0.05 for this theme): every click's broad displacement
  component grew ~×38/second. Proven numerically with the engine's own
  solver in isolation (512² grid, one click): peak 2.39 → inf at t = 27 s
  on f32; on the GL tier's RGBA16F targets the 65504 overflow arrives
  ~3 s in — matching the report's timeline. After overflow the field is
  NaN: the refraction composite samples garbage ("glitching") and every
  later click adds −2.4 to NaN ("nothing happens"). The same runaway kept
  every grid cell above the flat-water fast path forever, which is the
  Vulkan/CPU core-pinning measurement above. Fix — the stable damped form
  `nh = 2h − prev + 0.24·lap − dv·(h − prev) − ga·h` (all poles on/inside
  the unit circle; `ga` drains the DC/level mode velocity damping alone
  leaves), an absorbing border band (24 cells, taper to 0.70 — waves die
  at the screen edges instead of sloshing), a sub-visible fade (×0.94
  under height 0.08), a momentum-gated snap-to-zero, and a quiescence
  tracker so a calm lake is skipped entirely. Three sub-bugs found while
  tuning, all now pinned by tests: (1) a large snap deadzone (1e-2)
  deleted the small radiated halo around decaying clusters and the
  truncated cores sustained themselves as wandering ~0.04 limit cycles;
  (2) even a tiny deadzone catapulted — zeroing a trough while `prev`
  still carried momentum regenerated (0, −5.6e-4) → +5.2e-4 → +7.5e-4 —
  a self-recharging ~780-cell micro-ecology at 7e-4 amplitude (fixed by
  gating the snap on BOTH buffers being at the floor); (3)
  velocity-only damping leaves a permanent invisible ~0.05 "water level"
  offset (the DC pole sits at exactly z = 1). Measured after the fix: one
  click peaks at 2.4× its amplitude, stays visibly alive ~10–14 s, and the
  field is EXACTLY zero everywhere by ~17 s (no NaN is ever reachable);
  the idle CPU tier reports sim = 0.0 ms. Verified by: a standalone
  reproduction harness (before: ×38/s growth to inf; after: bounded,
  quiescent), new unit checks (waves visible → exact pass-through when
  calm → click-after-calm still splashes on target), new e2e tests
  `ripple(click-after-calm,cpu/opengl)` firing a click at t = 19 s after
  the first (both PASS, GPU/CPU centroids within 1 px), all existing
  click-placement/parity tests, and ASan/UBSan daemon smokes on the CPU
  and Vulkan tiers. The GL shader mirrors the CPU constants exactly
  (dv, ga, band, edge, fade, snap) so parity holds.
  Found along the way: the explicit-renderer failure path (e.g.
  `-R vulkan` with no ICD) leaked the framebuffers + scene/theme (4 MB
  per attempt, ASan) — now a full cleanup like the no-window path.
* **Ripple-lake clicks made a weak displaced smear instead of a wave
  (2026-10 postmortem)** — field report: after one click on
  `themes/ripple-lake` (Wayland, GTX 1050 Ti, GL tier with GPU sims), the
  effect "shifted toward the side of the screen and began glitching".
  Root cause, found by field-readback (`REV_RIPPLE_DUMP`) on llvmpipe and
  reproduced in isolation: the GPU ripple update shader folded the pointer
  drip and click impacts into the wave-update pass as LOCAL variables while
  sampling the Laplacian's neighbors from the UN-injected texture — the
  center term used the post-injection height and the neighbors did not. For
  a click of depth d the Laplacian gained a spurious −4d, so
  `nh = 2d + 0.24·(−4d) ≈ 1.04d`: the wave launch cancelled and the field
  decayed like a diffusion blob (probe: nh = −2.484 for a −2.4 click; the
  correct value is −4.8). The visible artifact was that blob's displaced
  specular glint — x tracked the click, y sat ~20–40 px high and the ring
  never expanded. Fix: a separate SPLAT pass writes injections into the
  field BEFORE the wave passes (exactly the CPU solver's order), and the
  wave pass is now a pure update. Verified: GPU splash 7,548 changed px
  centered on the click (was 420 px, displaced), CPU-vs-GPU parity 0.9 %,
  and e2e now has dedicated click-placement tests (`ripple(click,cpu)`,
  `ripple(click,gl)`, `ripple(click,corner)` in `scripts/e2e.sh`, comparator
  `scripts/click_cmp.py`). Found along the way, same investigation: a
  same-frame double click collapsed onto the LAST click's position (the
  pending-click slot was a single pair, not a queue) — the platform layer
  now latches up to 4 clicks with their own positions and the scene drains
  them individually.
* **"GPU timer queries available" at init, then `gpu=0.0ms*` forever
  (2026-10 postmortem)** — field report: on the GTX 1050 Ti the GL tier
  logged timer queries as available, yet `REV_PROFILE` reported
  `gpu=0.0ms*` with "timing unavailable". Three stacked defects: (1) the
  64-bit result entry point was looked up under the desktop-flavored name
  `glGetQueryObjectu64vEXT` — the ES extension spec name is
  `glGetQueryObjectui64vEXT` — so on ES contexts the lookup depended on
  drivers exporting a name that does not exist in that spelling;
  (2) detection trusted symbol resolution alone — no extension-string
  check, no functional probe — so "available" could be claimed by a context
  that would never yield results; (3) the frame loop re-armed a single
  query object before its previous result was retrieved, which is a GL
  INVALID_OPERATION and, once it happened, permanently stalled collection
  (the observed 0.0ms*). Fix: extension string + all four spec spellings +
  a real begin/end/finish/result probe at init (only a passing probe logs
  "available"; a failing one logs the honest reason), a 4-deep query ring
  whose slots are only re-armed after their results are collected, and a
  600-frame miss watchdog that disables timing with the driver-issue
  explanation instead of printing 0.0 forever. Also clarified in the perf
  note: `frame=` includes the pacing wait — when fps is capped to the
  display refresh, frame ≈ refresh interval and the remainder is vsync
  idle, not render cost.
* **Ripple scene + GL tier without GPU sims = black screen (2026-10
  postmortem)** — found by the scene-parity sweep: `-R opengl` on a ripple
  scene under llvmpipe (GPU sims declined) rendered nothing. In
  CPU-compositing mode main.c never calls `begin()`, and the GL tier's
  present path sized its viewport from window state that only `begin()`
  sets — a 0×0 viewport. Every other scene type composites in GPU mode, so
  nothing had ever exercised the combination. Fix: main.c now calls
  `begin()` in CPU-compositing mode too (which also fixes multi-monitor
  window targeting for that path), and the GL `end(cpu_frame)` path makes
  itself current and sizes itself from the frame independently. Parity
  sweep after the fix: ripple-lake GL vs CPU 0.00 % differing.
* **Theme asset cache never cached + leaked (2026-10 postmortem)** — the
  sanitizer audit caught a folder-theme asset leak: `theme_asset()` wrote
  its cache entry but never incremented the entry count — the cache could
  never hit (every lookup re-read the file from disk and overwrote slot 0,
  leaking the previous entry), and `theme_free()` freed nothing. One-line
  fix plus `ncache++`. Same audit: the packed-theme video path leaked its
  custom AVIO context + 64 KB buffer per open (avformat_close_input leaves
  custom-IO contexts alone), and the X11 screenshot hook never freed its
  persistent GetImage reply buffer at teardown — both now owned and freed
  by their teardown paths. Full suite re-verified under ASan+UBSan with
  leaks on.
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
  GPU compute — which both GPU tiers now do (`-R opengl`, and `-R vulkan`
  since 0.2.0; the CPU tier keeps its documented single-core cost).
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
