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
| input                  | QueryPointer polling        | wl_seat/wl_pointer events                |
| GPU present            | EGL (raw XID) or Vulkan (runtime-dlopen'd libxcb) | experimental WSI shim (`REV_WAYLAND_GPU=1`) |

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
| `REV_VK_PRESENT`    | `wsi` = vulkan tier presents via its swapchain (default: platform SHM path) |
| `REV_WAYLAND_GPU`   | `1` = activate the experimental Wayland WSI shim    |
| `REV_WL_TRACE`      | `1` = dump raw Wayland wire traffic to stderr (diagnostics) |

## Troubleshooting

* **"compositor lacks ... zwlr_layer_shell_v1"** — GNOME Wayland, or a
  compositor without layer-shell (stock weston is another). See the caveat
  above; use X11 (XWayland counts as X11) or a wlroots/KDE compositor.
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
  benchmark it.
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
