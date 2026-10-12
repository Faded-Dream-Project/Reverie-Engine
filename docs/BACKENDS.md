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
* Per frame: engine composites on the CPU → memcpy into a host-visible
  staging buffer → `vkCmdCopyBufferToImage` with layout barriers → FIFO
  present. SRGB surface formats are accepted: the copy path moves raw bytes
  (no sampling), so they are visually identical to UNORM here.
* Verified E2E on lavapipe: **pixel-exact vs the CPU renderer.**

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
| `REV_WAYLAND_GPU`   | `1` = activate the experimental Wayland WSI shim    |

## Troubleshooting

* **"compositor lacks ... zwlr_layer_shell_v1"** — GNOME Wayland. See the
  caveat above; use X11 (XWayland counts as X11) or a wlroots compositor.
* **"vulkan unavailable (...)"** — read the reason in the log: no loader,
  no ICD, no presentable queue family, or the surface does not allow
  TRANSFER_DST usage (the engine falls through to GL).
* **"opengl unavailable (...)"** — usually no libEGL or no EGL vendor; on
  headless test setups set `__EGL_VENDOR_LIBRARY_FILENAMES` if you keep
  Mesa outside the default paths.
* **llvmpipe note in the log** — expected on software GL; it is a fully
  supported tier, not an error.
