# Reverie Engine — Architecture

One process, one thread, zero locks. Data flows in one direction:

```
 .rev file ──rev.c──▶ IR ──scene.c──▶ RTScene ──┐
                                               │ per frame
 pointer ──x11.c──▶ per-monitor pointer state ─┤
                                               ▼
                    ┌──────────── scene.c: rtscene_render ────────────┐
                    │ for each layer (bottom → top):                   │
                    │   anims → effective props                        │
                    │   content raster (cached / simulated)            │
                    │   blur (cached for static layers)                │
                    │   blend into accumulator (composite.c)           │
                    └───────────────────────┬──────────────────────────┘
                                            ▼ RGBA8 straight-alpha
                    ┌──────── x11.c: x11_present ─────────────────────┐
                    │ convert to visual layout → SHM segment →        │
                    │ ShmPutImage (double-buffered) → X server window │
                    └─────────────────────────────────────────────────┘
```

## Modules

| File            | What it does |
|-----------------|--------------|
| `arena.c`       | bump allocator; scene IR and SVG DOMs live in arenas so hot-reload is one `arena_destroy` |
| `log.c`         | leveled logging to stderr |
| `util.c`        | file I/O, string/path helpers, color parsing, easing curves, monotonic time |
| `simd.c`        | CPU feature detection (SSE2 baseline; AVX2 probe) |
| `inflate.c`     | complete RFC 1951 DEFLATE inflater — single-level 15-bit Huffman tables |
| `png.c`         | PNG decode (all color types/depths/Adam7) + encode (stored blocks) + CRC32 |
| `geom.c`        | 2×3 affine transforms, path containers, adaptive bezier flattening, arc→cubic |
| `raster.c`      | scanline AA fill (exact horizontal coverage, 4× vertical supersampling, nonzero/evenodd), gradient LUTs, full stroker (joins/caps/dashes) |
| `svg.c`         | hand-rolled XML, style/presentation resolution, full path grammar, viewBox/preserveAspectRatio, group opacity, gradients |
| `rev.c`         | `.rev` lexer + recursive-descent parser → typed IR with line/col errors |
| `scene.c`       | validation/compilation, animation evaluation, per-monitor layer instances, raster/blur caching, parallax oversizing, the render pipeline |
| `composite.c`   | W3C separable blend modes, 3-pass premultiplied box blur, bilinear resampling |
| `fluid.c`       | Stam "stable fluids": pressure projection (Gauss–Seidel), semi-Lagrangian advection, vorticity confinement, dye at 2× velocity resolution, ambient forcing |
| `ripple.c`      | wave-equation height field; refraction of the below-composite + specular highlights |
| `particles.c`   | deterministic PRNG particle system with ambient presets and pointer bursts |
| `x11.c`         | **the raw X11 wire client** (see below) |
| `main.c`        | the daemon: arg parsing, monitor setup, frame loop, pointer polling, hot-reload |
| `revtool.c`     | CLI: validate / info / offline render (with simulated pointer+drag) / mkpng / pnginfo |

## Pixel convention

Everything internal is **RGBA8, straight alpha**, packed `u32`
(`a<<24 | b<<16 | g<<8 | r`). Compositing follows the W3C blend model

```
Co = αs(1−αb)·Cs + αsαb·B(Cb,Cs) + (1−αs)αb·Cb
Ao = αs + αb(1−αs)
```

with `B` being the blend function (`normal add screen multiply overlay`).
Blur premultiplies internally to avoid dark halos. Conversion to the X
visual's channel masks happens once, in `x11_present`.

## The raw X11 client

`x11.c` speaks the X11 wire protocol directly — no Xlib, no XCB:

- **connect**: parse `DISPLAY`, open `/tmp/.X11-unix/X<n>`, optionally send
  a MIT-MAGIC-COOKIE-1 from `~/.Xauthority` (or `$XAUTHORITY`)
- **setup**: parse the server's setup reply — resource ID range, root
  window, formats, and a TrueColor visual (preferring the root depth),
  computing per-channel shifts/losses from the RGB masks
- **requests**: little-endian, 4-byte aligned, sent with a tiny `send_req`;
  replies are read in strict lockstep (events encountered on the way are
  buffered and dispatched by `x11_pump_events`)
- **windows**: `CreateWindow` + `CreateGC` + EWMH atoms interned and set
  (`_NET_WM_WINDOW_TYPE_DESKTOP`, `STICKY`, `BELOW`, `SKIP_TASKBAR/PAGER`)
  + `MapWindow`
- **presentation**: two MIT-SHM segments per window, ping-ponged; each
  frame converts pixels into the segment and issues `ShmPutImage`; a cheap
  `GetInputFocus` round-trip guarantees the server finished reading a
  segment before it is reused. If MIT-SHM is unavailable it falls back to
  chunked core `PutImage`
- **monitors**: Xinerama `QueryScreens` (with the `QueryVersion` handshake
  real servers require), falling back to full-screen root
- **input**: `QueryPointer` polling on the root window per frame —
  position *and* button mask, no event round-trips needed
- **testing**: `x11_screenshot` uses `GetImage` to read the window back,
  which is how the automated Xvfb test proves byte-exact presentation

Notable wire-format details (all discovered the hard way, against a live
server): the setup reply's length is a u16 at offset 6; X.Org emits the
screen/format counts as single bytes; extension requests carry their minor
opcode in the request's second byte; `ShmPutImage` on X.Org is 40 bytes
with four pad bytes before the depth field — captured by relaying a real
Xlib client through a byte-logging socket proxy.

## Caching strategy

Static layers (image/gradient/solid) rasterize once per (size, content) —
including their *blurred* variants; animating opacity or blend reuses the
cached raster. Gradients with an animated `angle` re-render each frame
(they're cheap). Fluids, ripples, and particles re-simulate every frame by
nature. Parallax renders the layer slightly overscanned
(`1 + parallax·0.08`) and shifts it under the pointer.

## Testing

`make test` runs four suites:

- `test_inflate` — streams produced by independent zlib runs (stored /
  fixed / dynamic blocks, raw wrappers, truncation, bad checksums)
- `test_png` — every color type × depth × filter × interlace combination
  against oracle files, plus encoder round-trip
- `test_svg` — golden-pixel scenarios (shapes, gradients, transforms,
  fill-rule, strokes, arcs, group opacity)
- `test_scene` — full pipeline: parse → compile → render frames with
  simulated pointer input; asserts on composited pixels

The end-to-end Xvfb test (render 90 frames, `GetImage` readback,
pixel-exact compare) lives in the repo history and is reproducible with
any `Xvfb :99` instance.

## Deliberate limitations

- v1 targets X11; the core is backend-agnostic (scene → RGBA buffer), so a
  Wayland `wlr-layer-shell` presenter is an isolated addition
- SVG covers the core set — no `<text>`, masks, patterns, `use`, or CSS
  class selectors
- stroking approximates uniform width under non-uniform transforms
- image formats: PNG and SVG (JPEG is a compression format for photos of
  cats, not for wallpaper engines)
- single-threaded by design; the frame budget is met through caching, not
  core count
