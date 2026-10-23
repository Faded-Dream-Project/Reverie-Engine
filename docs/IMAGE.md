# Static image wallpapers — `--image`

`reverie --image <file.png>` runs a single PNG as the wallpaper: the
static sibling of `--video` media mode. No `.rev` file, no scene system,
and — unlike video — **no FFmpeg anywhere in the path**: the decode goes
through Reverie's own native PNG codec (`src/png.c`, see
[PNG.md](PNG.md)), so image wallpapers work in every build
configuration, `ENABLE_FFMPEG=0` stub builds included.

```sh
reverie --image walls/dusk.png            # fill (cover + crop) — default
reverie --image walls/dusk.png --scale fit      # letterbox on black bars
reverie --image art/poster.png --scale center   # native size, centered
reverie --image wide.png --scale stretch        # ignore aspect, fill all
reverie --image walls/dusk.png --scale fit -o   # offline: one PNG out, no display
```

## Placement — the shared media vocabulary

`--scale` is the SAME placement vocabulary as `--video` (position
independent, `--scale` may come before or after `--image`):

| mode     | behavior                                                        |
|----------|-----------------------------------------------------------------|
| `fill`   | cover the viewport, center-crop the excess (**default**)        |
| `fit`    | contain, centered; letterbox/pillarbox bars are opaque black    |
| `stretch`| fill the viewport ignoring aspect ratio                         |
| `center` | native size, centered; crops symmetrically if larger than screen |

Media-mode semantics apply unchanged: the background is OPAQUE black
(video-player convention — the desktop never shows through), transparent
texels in the PNG composite over that black, and the visible-region
bounding keeps memory proportional to the viewport: a 100×10000 source
under `fill` converts only the strip the screen actually shows, never a
1920×192000 intermediate.

`--no-loop` is rejected next to `--image` — a static image has no loop
to decline; it is `--video`-only.

## What the renderer does

The image is decoded once, synchronously, at startup. From there:

* **GPU tiers (Vulkan / OpenGL)** receive the image as one pixel layer
  with a stable content id — the texture is uploaded ONCE and rebound
  for every later frame. A static wallpaper costs a single upload per
  renderer, however many hours it runs.
* **CPU tier** places with the engine's own bilinear resampler
  (`rev_resample_bilinear_sub` — premultiplied interpolation, no alpha
  halos), straight into the framebuffer.
* **Pacing**: a static image runs at 2 fps (nothing changes between
  frames; the GPU tier is a cached-texture rebind and the CPU tier
  re-places twice a second) while the IPC pump stays responsive.
  `--fps N` overrides if you want it slower or faster.
* **IPC**: `STATUS` reports `mode=image path=... renderer=...
  backend=... visible=... scale=...`; `PING`/`QUIT` behave as usual;
  scene-mode commands (`THEME`, playlists) report the same clear
  "restart with a scene" error as video mode.

## Decoder guarantees

The native codec decodes every standard combination of color type
(truecolor, grayscale, palette, with/without alpha, tRNS), bit depth
(1/2/4/8/16) and Adam7 interlacing into straight-alpha RGBA8, and
enforces hard caps: dimensions up to 16384, at most 100 megapixels,
bounded inflate — a decompression bomb or a truncated file is a clean
`reverie: PNG: ...` error and exit 1, never a crash. The full decoder
contract, including the negative-test suite, is documented in
[PNG.md](PNG.md).

## Session memory and autostart

An image launch is a first-class content mode: the daemon records
`--image <absolute path>` (plus `--scale` when you spelled it out) into
`~/.local/state/reverie/last-session.json`, exactly like a theme or
video launch. `reverie --restore` replays it through the real argument
parser, and an enabled XDG autostart entry (see
[AUTOSTART.md](AUTOSTART.md)) therefore restores your last image
wallpaper at login with nothing extra to configure.
