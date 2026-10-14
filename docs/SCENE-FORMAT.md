# The `.rev` Scene Format

A Reverie scene is a single text file describing a stack of layers, how they
are composited, and how they react to you. The syntax is deliberately small:
`key = value` properties inside `layer` blocks, values being numbers,
strings, colors, tuples, and lists. Semicolons and commas are optional
where they add no clarity.

Comments are `// line` and `/* block */`.

## Top-level structure

```
scene "Optional Title" {
    fps = 60

    layer <name> { ... }
    layer <name> { ... }
}
```

The `scene "Title"` header and the outer braces are optional — a file that
is just a list of `layer` blocks is also valid.

| Property | Type  | Default | Meaning                                    |
|----------|-------|---------|---------------------------------------------|
| `fps`    | number| 30      | target frame rate, 1..240                   |

## Value types

| Example                | Kind   |
|------------------------|--------|
| `1` `0.5` `-3`         | number |
| `6s` `250ms`           | duration (seconds / milliseconds) |
| `50%`                  | percent (context-dependent) |
| `"assets/sky.png"`     | string (paths are relative to the .rev file) |
| `#abc` `#rrggbb` `#rrggbbaa` | color |
| `(0.5, 0.75)`          | tuple |
| `[#111, (#fff, 0.8)]`  | list (of colors / (color, offset) pairs) |
| `fill` `screen` `true` | identifier / boolean |

## Layer common properties

| Property   | Type    | Default  | Meaning                          |
|------------|---------|----------|----------------------------------|
| `type`     | ident   | required | `image gradient solid fluid ripple particles video` |
| `opacity`  | number  | 1.0      | 0..1, animatable                 |
| `blur`     | number  | 0        | gaussian-ish blur radius in px, animatable |
| `blend`    | ident   | normal   | `normal add screen multiply overlay` |
| `parallax` | number  | 0        | 0..1, pointer-driven depth       |
| `monitor`  | ident/number | all | `all` or monitor index           |
| `smooth`   | bool    | true     | bilinear vs nearest scaling      |

Layers composite **bottom to top** (file order), each with source-over or
the chosen W3C-style blend function over the accumulated result.

## `type = image`

| Property | Type   | Default | Notes                                   |
|----------|--------|---------|-----------------------------------------|
| `src`    | string | required | `.png` or `.svg` relative to the scene  |
| `fit`    | ident  | `fill`  | `fill` (cover) `fit` (contain) `stretch` `tile` |

SVGs re-render natively at the target resolution (viewBox-aware); PNGs are
resampled bilinearly (or nearest with `smooth = false`).

## `type = gradient`

```
layer sky {
    type = gradient
    shape = linear          // or radial
    angle = 180             // degrees, linear only, animatable
    center = (0.5, 0.6)     // radial center, relative
    radius = 0.8            // radial radius, relative
    stops = [#0b0b2a, (#b4486d, 0.45), (#ff9e64, 0.9)]
}
```

Stops are colors, or `(color, offset)` pairs; bare colors are evenly
spaced. Offsets are 0..1 along the gradient vector.

## `type = solid`

```
layer flash { type = solid  color = #ff00ff80 }
```

## `type = fluid` — the interactive one

A real-time Navier–Stokes fluid. The pointer injects momentum and dye.

| Property      | Type   | Default   | Meaning                               |
|---------------|--------|-----------|---------------------------------------|
| `res`         | number | 0.3       | sim resolution scale (0.05..1)        |
| `dye`         | color  | #ff5ec4   | primary dye color                     |
| `dye2`        | color  | #4cb8ff   | secondary dye (movement direction)    |
| `force`       | number | 8         | pointer force, animatable             |
| `radius`      | number | 0.015     | splat radius (relative), animatable   |
| `dissipation` | number | 0.12      | dye fade rate 0..1                    |
| `ambient`     | number | 0         | self-stirring breeze 0..1             |

Blends beautifully as `blend = screen` over dark skies.

## `type = ripple`

A wave-equation water surface. The pointer depresses the surface; the layer
refracts everything composited *below it* and adds specular glints.

| Property   | Type   | Default | Meaning                  |
|------------|--------|---------|--------------------------|
| `strength` | number | 1.0     | wave + refraction power, animatable |
| `damping`  | number | 0.12    | how fast ripples die     |
| `radius`   | number | 0.01    | pointer disturbance size |

## `type = video`

An MP4 (H.264) video layer — looping wallpaper footage that composites
like any other layer (blend/opacity alongside everything else). Decoding
is optional: without FFmpeg on the system the layer logs one line and
renders nothing (see docs/VIDEO.md).

```
layer loop {
    type = video
    src = "videos/loop.mp4"
    loop = true
    fit = fill
    paused = false
}
```

| Property  | Kind    | Default    | Meaning                                     |
|-----------|---------|------------|---------------------------------------------|
| `src`     | string  | required   | video file, theme-relative                  |
| `loop`    | bool    | `true`     | restart at EOF                              |
| `paused`  | bool    | `false`    | hold the current frame (delta-time clock)   |
| `fit`     | ident   | `fill`     | `fill` / `fit` / `stretch` aspect handling  |

Playback timing is PTS-driven on the layer's own playhead — changing the
render fps does not change playback speed. Audio tracks are never opened
(a wallpaper has no business making sound). Packed themes stream the
video directly out of the container.

## `type = particles`

| Property  | Type   | Default  | Meaning                              |
|-----------|--------|----------|--------------------------------------|
| `preset`  | ident  | custom   | `rain snow embers petals fireflies`  |
| `spawn`   | ident  | ambient  | `ambient pointer both` (pointer = click bursts) |
| `rate`    | number | 60       | particles per second                  |
| `size`    | number | 3        | px                                    |
| `gravity` | number | 60       | px/s² (negative rises)                |
| `drift`   | number | 20       | horizontal wander                     |
| `color`, `color2` | color | warm | color range lerped per particle |

## Animations

Any numeric property marked *animatable* above can carry keyframe anims:

```
layer mist {
    ...
    anim opacity {
        from = 0.3
        to   = 0.6
        dur  = 14s        // also plain seconds: dur = 14
        delay = 2s        // optional
        ease = sine       // linear sine quad cubic quart expo back elastic bounce
        mode = pingpong   // once | loop | pingpong
    }
}
```

Multiple `anim` blocks on one layer animate different properties
independently. Animated properties replace their base value during playback.

## Full example

```
scene "Mountain Dusk" {
    fps = 60

    layer sky {
        type = gradient
        shape = linear
        angle = 180
        stops = [#0a0a2a, (#432b6b, 0.45), (#b4486d, 0.72), (#ff9e64, 0.9), (#2b1a3d, 1.0)]
    }

    layer mountains {
        type = image
        src = "assets/mountain.svg"
        fit = fill
        parallax = 0.14
    }

    layer mist {
        type = fluid
        res = 0.35
        dye = #7fd4ff
        dye2 = #ffb8d9
        force = 10
        radius = 0.022
        dissipation = 0.28
        ambient = 0.4
        blend = screen
        opacity = 0.45
        blur = 5
        anim opacity { from = 0.3; to = 0.6; dur = 14s; ease = sine; mode = pingpong }
    }
}
```

## Error reporting

Errors carry line numbers and say what they expected:

```
$ revtool validate scenes/myscene.rev
FAIL: layer 'sky' (line 14): gradient needs stops = [color, ...] (min 2)
```


---

## The .revpack container

A `.revpack` bundles a scene's text with every referenced asset (PNG/SVG)
into one checksummed binary, so a wallpaper ships as a single file. The
daemon accepts `.rev` and `.revpack` interchangeably (it sniffs the magic).

All integers are little-endian.

```
offset  size  field
0       6     magic "REVPAK"
6       2     u16 format version (currently 1)
8       4     u32 scene_off        offset of the scene text
12      4     u32 scene_len        scene text length (excluding NUL)
16      4     u32 asset_count
20      4     u32 name_blob_off    name/strings region offset
24      4     u32 name_blob_len
28      4     u32 data_off         asset data region offset
32      4     u32 header_crc32     CRC-32 of bytes 0..31
36      3     u8[3] writer engine version (e.g. 0.1.0 → 0,1,0)
39      1     u8 flags (0)
40      28×N  asset entries:
              u32 name_off, u32 name_len        (into the name blob)
              u64 data_off, u64 data_len
              u32 data_crc32                    (over the asset bytes)
...           name blob (NUL-terminated names, concatenated)
...           scene text (NUL-terminated)
...           asset data blobs
```

Loader rules: magic and version must match; every offset/length is bounds
checked against the file; the header CRC and every asset CRC are verified;
assets are addressed by the exact path string used in the scene's `src` /
`image` properties. Missing assets in a pack fall back to disk with a
warning (packs are built best-effort).

Tooling:

```
revtool pack   scenes/mountain-dusk.rev -o dusk.revpack
revtool unpack dusk.revpack -d dusk_dir/     # scene.rev + flat assets
reverie dusk.revpack                          # runs directly from the pack
```
