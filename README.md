# Reverie Engine

**An interactive wallpaper engine written in pure C. The only things it links
are libc, libm, and the Linux kernel.**

Reverie is *not* a wallpaper setter — it does not change your desktop
background image. It is the **engine**: it opens real desktop windows behind
your icons (EWMH `_NET_WM_WINDOW_TYPE_DESKTOP`), renders animated,
GPU-less scenes into them at full frame rate, and lets you **interact with
your wallpaper** — drag your pointer through a living fluid, drag ripples
across water, spark embers with a click.

Scenes are described in the `.rev` scene language: layered PNGs, SVGs,
gradients, fluid/ripple/particle sims, blend modes, blur, opacity, parallax,
and keyframe animations — all in a small, hand-editable text file.

```
 ~ $ reverie scenes/mountain-dusk.rev
 [info] scene 'scenes/mountain-dusk.rev': 5 layer(s), 60 fps, animated
 [info] 2 monitor(s)
        ... and your desktop is alive.
```

---

## The absurd dependency list

| Component           | Other engines use   | Reverie uses                          |
|---------------------|---------------------|---------------------------------------|
| Window system       | Xlib / XCB / GTK    | **raw X11 wire protocol over a socket** |
| Compression         | zlib                | **own DEFLATE inflater (RFC 1951)**   |
| Images              | libpng / libjpeg    | **own PNG codec (all color types, Adam7)** |
| Vectors             | librsvg / cairo     | **own SVG rasterizer + stroker**      |
| Shared memory       | Xext wrappers       | **MIT-SHM via sysv shm, hand-encoded** |
| Threads             | pthreads            | **single-threaded, zero locks**       |
| Font rendering      | fontconfig/freetype | *(out of scope for the core set)*     |

Everything is C11, compiled with `-Wall -Wextra -Werror -Wpedantic`, and
verified end-to-end: an automated Xvfb test renders 90 frames through the
complete pipeline (scene → compositor → wire protocol → X server) and reads
the pixels back with `GetImage` — **byte-identical, 655360/655360 pixels**.

## Build

```sh
make            # build/reverie + build/revtool
make test       # run the test suite (inflate, PNG, SVG, scene integration)
make assets     # regenerate scene assets using Reverie's own PNG encoder
make install    # /usr/local/bin/{reverie,revtool}
```

Requirements: any C11 compiler and make. That's it.

## Usage

```sh
reverie [options] <scene.rev>

  -f, --fps N        override the scene's target fps
  -m, --monitor N    render only on monitor N
  -o, --once         render one frame to a PNG and exit (no X11 needed)
  -s, --size WxH     output size for --once (default 1920x1080)
  -O, --out FILE     output PNG path for --once
  -l, --list         list monitors and exit
  -v / -q            more / less logging
```

While it runs, Reverie watches the `.rev` file: **save the file and the
wallpaper hot-reloads** in place.

## revtool

```sh
revtool validate scene.rev                 # parse + compile, precise errors
revtool info scene.rev                     # dump the parsed scene tree
revtool render scene.rev --size 1920x1080 --frames 120 --fps 60 \
        --pointer 0.5,0.5 --out frame_%04d.png     # offline render (tests interaction!)
revtool mkpng out.png 1024 640 soft-bands  # procedural asset generator
revtool pnginfo asset.png                  # PNG metadata dump
```

`revtool render` is a full offline harness — it can simulate a held pointer
or a drag gesture so you can preview exactly how a scene reacts before
ever putting it on your desktop.

## Scene features

- **Layers**: `image` (PNG + SVG), `gradient` (linear/radial, multi-stop),
  `solid`, `fluid`, `ripple`, `particles`
- **Per layer**: opacity, blur, blend mode (`normal add screen multiply
  overlay`), parallax depth, monitor binding, smooth/nearest scaling
- **Interactions**: pointer paints dye and momentum into the Navier–Stokes
  fluid; drags ring wave-equation ripples that refract the scene beneath
  them; clicks and motion spawn particle bursts
- **Animations**: keyframe anims on opacity/blur/parallax/force/angle/…,
  9 easing curves, `once | loop | pingpong` modes
- **SVG core set**: full path grammar (incl. elliptical arcs), shapes,
  groups, transforms, viewBox + preserveAspectRatio, fill/stroke with
  opacity and dashes, linear/radial gradients, ~150 named colors
- **PNG decoder**: every color type (0/2/3/4/6), bit depths 1–16, tRNS,
  Adam7 interlacing — verified byte-exact against independent oracles
- **Multi-monitor**: one wallpaper window per monitor (Xinerama), each with
  its own sim state; layers can target `monitor = 2`

See [docs/SCENE-FORMAT.md](docs/SCENE-FORMAT.md) for the complete scene
language reference and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for
how it all works inside.

## Bundled scenes

| Scene                    | Shows off                                            |
|--------------------------|------------------------------------------------------|
| `scenes/fluid-aurora.rev`| pure gradients + bright interactive fluid            |
| `scenes/mountain-dusk.rev`| hand-drawn SVG ridgeline, parallax, drifting mist   |
| `scenes/ember-drift.rev` | particle embers, click to spark                      |
| `scenes/ripple-lake.rev` | refraction ripples you can drag across the water    |
| `scenes/snowfall.rev`    | blurred backdrop + snow + frost vignette             |

## Performance notes

The renderer is pure CPU with a few deliberate tricks: static layers
(images, gradients) are rasterized once and cached (including their blurred
versions); only simulations re-render per frame; the fluid dye field runs
at double the velocity resolution for crisp visuals; scansline coverage is
exact horizontally. On a 2-core box, 1080p interactive scenes hold 60 fps.
`reverie -f 30` halves the cost if you want headroom.

## Platform scope

Linux + X11 (any EWMH-respecting window manager). Wayland support would
require a `wlr-layer-shell` backend — the engine core is backend-agnostic
(scene → RGBA buffer), so it is a matter of writing the presenter.

## License

Proprietary — All Rights Reserved. See [LICENSE](LICENSE).
