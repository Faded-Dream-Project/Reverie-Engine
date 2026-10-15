# Reverie Video Wallpapers — Architecture Assessment & Prototype

Status: **assessment complete; phase-1 prototype implemented** (software
decode through dlopen'd FFmpeg libraries, GPU color conversion, bounded
frame queue, looping). This document records the trade-off analysis that
chose that architecture and what later phases would add.

---

## 1. The actual problem space

"MP4 support" is not one problem. An MP4 is a **container** (the ISO
BMFF 'moov'/'mdat' box structure: timescales, track tables, edit lists,
fragmented vs progressive layouts). Inside it, a **codec** does the real
work. A wallpaper engine that accepts real-world files faces:

* **H.264/AVC** — the default codec of every phone camera and 90% of
  wallpaper loops. Decoding it means: NAL parsing, exp-Golomb entropy
  coding, **CAVLC and CABAC** arithmetic decoding, intra prediction (9
  luma + 4 chroma modes), inter prediction with sub-pel motion
  compensation (6-tap quarter-pel filters), deblocking across slice
  boundaries, B-frame reordering, and weighted prediction. A conformant
  *baseline-only* implementation is ~30-60k lines of subtle C; Main/High
  (what real files actually use) adds CABAC, 8x8 transforms, and more.
* **H.265/HEVC** — larger and harder (35 intra modes, CTU quad-trees,
  SAO), with a notoriously unresolved multi-pool patent situation.
* **VP9 / AV1** — patent-unencumbered but heavier still; AV1's
  film-grain synthesis alone is a subsystem.
* Plus: timestamps (variable frame rates, PTS/DTS reordering), seek
  tables, color conversion (YUV limited/full range, BT.601/709), and
  container-level corruption tolerance.

**Conclusion: writing a from-scratch codec is a multi-month project
whose output would still fail on ordinary files.** It fails the engine's
"reliable, small, honest" identity. The container side (MP4 demuxing) is
 tractable (~2k lines) but worthless without a decoder behind it — so
a "custom MP4 demuxer + custom H.264 decoder" plan is really just "a
custom H.264 decoder" plan.

## 2. Options compared

| Approach | Deps at build | Deps at run | Scope | Verdict |
|---|---|---|---|---|
| **A. dlopen FFmpeg (libavcodec/format/util/swscale)** | none | optional | ~700 lines in our code, world-class decode | **chosen (phase 1)** |
| B. link FFmpeg | `-lav*` | required | same as A | breaks the zero-dependency build — rejected |
| C. GStreamer (dlopen or link) | none / gst | larger surface | pipelines, app sinks, plugin registry quirks | more moving parts than a wallpaper needs |
| D. custom H.264 baseline decoder | none | none | 30-60k+ lines, years of conformance fixes | rejected (see §1) |
| E. platform decode (VA-API / NVDEC / VDPAU) | libva / cuda | driver-specific | hardware paths, interop complexity | phase 2+, layered ON A |
| F. image sequences (PNG frames) | none (we have PNG) | none | huge RAM/disk, no audio-visual quality | supported trivially via scenes; not "video" |

Why A over C: FFmpeg's libraries present a small flat C API
(`avformat_open_input` → `avcodec_send_packet` → `avcodec_receive_frame`)
that maps exactly onto our dlopen style (identical to how EGL and Vulkan
are already loaded). GStreamer would add an object system, a plugin
registry, a main-loop-ish bus model, and versioned ABI churn across
distros — for zero wallpaper-relevant capability we don't get from A.

Why E is not phase 1: hardware decode changes the frame TYPE
(`AV_PIX_FMT_VAAPI`, `AV_PIX_FMT_CUDA`) — pixels live in driver
allocations. Getting them into GL requires EGL dmabuf import or the CUDA
/GL interop extension: driver-dependent code paths (Mesa vs NVIDIA) that
need their own validation matrix. Phase 1 instead uploads the decoded
YUV planes as textures and converts to RGB **in the composite shader** —
GPU work either way, no CPU color conversion, and identical code on
every GPU. The upload itself is bounded: 1080p YUV420P = 3.1 MB/frame
(vs 8.3 MB as RGBA — 2.7x less bandwidth than the naive path).

## 3. Licensing

* FFmpeg libraries: LGPLv2.1+ — compatible with a proprietary
  statically-linked-avoiding user, **provided we dlopen and keep the
  LGPL components dynamically replaceable**, which dlopen does by
  construction. (Building a commercial H.264 *encoder* or enabling
  nonfree libraries would change this; decoding for playback is the
  intended and standard use.)
* Proprietary codecs: AVC essential-pool royalties apply to
  *encoders/decoders shipped in products* in some jurisdictions; the
  practical reality for an engine that merely *uses the system's
  already-installed FFmpeg* is the same one adopted by every desktop
  player that ships without bundled codecs. A from-scratch decoder would
  NOT escape this (patents cover the techniques, not the code).
* This is engineering analysis, not legal advice; a commercial release
  should still get counsel for its jurisdictions.

## 4. What the prototype implements

* `src/video.c` — decoder loaded via `dlopen` with versioned sonames
  (`libavcodec.so.61/60/59/58`, etc.). **Missing libraries are a
  non-event**: the engine runs exactly as before; video layers log one
  diagnostic line and render nothing.
* Demuxing through a **custom AVIOContext** — folder themes read from
  the file path, packed themes read **directly out of the .revpack
  entry** (seek callback included for looping). No extraction.
* One **decode thread** per video layer (the engine's only thread —
  contained in video.c, pthread): decodes ahead of the playhead with a
  bounded queue (~6 frames), never blocks the render loop; the render
  thread only ever locks briefly to read the current frame's plane
  pointers.
* **Two frame families**, selected by what the decoder actually emits:
  - YUV420P stays on the plane path (Y/U/V borrowed pointers) — the
    GPU tiers upload them as R8 textures and convert in the composite
    shader (2.7x less bandwidth than RGBA).
  - everything else (animated GIF — PAL8 **or** BGRA depending on the
    FFmpeg build — PNG streams, RGBA MOVs) is normalized to packed
    straight-alpha RGBA **on the decode thread**, into a synthetic
    refcounted AVFrame with tight rows (linesize == w*4). The buffer is
    already in the engine's native 0xAABBGGRR packing on little-endian
    (sws `AV_PIX_FMT_RGBA` writes R,G,B,A bytes), so it doubles as a
    plain pixel layer — the CPU tier converts once via
    `rev_video_to_rgba_at()` straight into a placed rectangle
    (destination stride supported, no intermediate blit).
* **Looping is consumer-driven**: at EOF the decoder sleeps on a
  `wrap_pending` flag until the render side drains the queue and resets
  its own playhead. A decoder-side reset races the consumer — on files
  shorter than the queue depth (a 2-frame GIF) the decoder wrapped so
  fast the playhead never reached frame 1; on longer files the queued
  tail of cycle N played during cycle N+1. Demuxers without seek
  support (the GIF demuxer) reopen the input from byte zero instead of
  seeking; the swap happens under the queue lock. A paused or hidden
  daemon now stops decoding entirely instead of spinning through EOF.
* **Memory bound**: plane frames are refcounted (queue depth 6 is
  cheap); RGB-family sources hold full RGBA copies, so their queue
  depth is `48 MB / frame bytes`, clamped to [2, 6] — a 4K RGBA stream
  cannot queue 200 MB.
* **Playback timing** is driven by the layer's own playhead
  (delta-time, pause-aware) — rendering FPS does not change playback
  speed; GIF frame delays arrive as normal PTS and are honored exactly.
  **Audio: not opened.** The video stream is selected explicitly; audio
  tracks are ignored (a wallpaper has no business making sound).
* Scene language: `layer v { type = video; src = "videos/loop.mp4";
  loop = true; fit = fill; paused = false }` — a normal layer that
  composites with blend/opacity alongside everything else (animated
  GIFs work here too).
* **GL tier**: Y/U/V planes uploaded as R8 textures, BT.601
  limited-range YUV→RGB in a fragment shader during the standard
  composite pass (placement/blend/opacity shared with image layers);
  RGBA frames take the standard pixel-layer path.
* **CPU tier**: `sws_scale` straight into the placed rectangle —
  functional fallback, identical visual result.
* Resource safety: videos > 8192×8192 or broken streams are refused with
  a message; decode errors drop the layer (one diagnostic), never the
  engine; shutdown joins the decode thread.

## 4b. The `--video` media mode (2026-10)

`reverie --video PATH` plays a video or animated GIF as the wallpaper
**without any scene file** — the second, independent content mode. The
scene and media modes share only the infrastructure (backends,
renderers, presentation, IPC socket, screenshots, idle power-save);
neither requires the other.

* **CLI**: `--video PATH` (mutually exclusive with any scene —
  positional, `--scene`, or a second `--video` is a hard error),
  `--scale fill|fit|stretch|center` (default **fill**: cover the
  viewport, center-crop the excess), `--no-loop` (hold the last frame at
  EOF; the default loops), `--fps N` (pacing override).
* **Scaling math** (`rev_scale_place()` in geom.c, unit-tested in
  tests/test_scale.c): `fill` covers and crops (negative placement
  offsets are legal — the GPU tiers clip natively); `fit` contains and
  letterboxes; `stretch` ignores aspect; `center` keeps native size and
  crops symmetrically when oversized. **Letterbox bars are opaque
  black** — video-player semantics, byte-identical on every renderer
  (the CPU tier pre-fills 0xFF000000; the GPU tiers draw a cached 1x1
  black background layer). Transparent source texels (GIF) composite
  over that black — the desktop never shows through in media mode; the
  see-through-wallpaper trick remains a scene-mode feature.
* **Bounded CPU placement**: cover-crop never materializes the placed
  rect — a 100x10000 source under fill would be 1920x192000 — the
  VISIBLE source sub-rect is converted directly into the viewport
  (`rev_video_to_rgba_sub`), so allocation is bounded by the viewport.
  All conversions go through a padded scratch: sws can spill up to one
  extra output row past the last (measured: 8 u32 past the final row on
  sub-rect SIMD paths; FFmpeg-internal, varies by build). The tight
  `rev_video_to_rgba()` wrapper carries its own padding — the historical
  dw*dh-exact contract of every existing caller holds, and the latent
  write-past-the-buffer hazard in the scene-layer path is closed.
* **Playback**: the loop paces at the source's average frame rate
  (`rev_video_fps()`), so uploads happen at video speed; the playhead
  freezes while the surface is hidden (idle power-save) and resumes
  exactly where it stopped. Looping wraps at the LAST FRAME'S END
  (pts + duration), not when the decode queue drains — wrapping on a
  queue drain truncates the final frame to a single tick.
* **GPU tiers**: YUV frames ride the plane path (vsim); RGB-family
  frames ride the standard pixel-layer path. The CPU tier converts
  through the padded scratch and copies/composites into the framebuffer.
* **IPC**: `PING`/`STATUS`/`QUIT` work; `STATUS` reports `mode=video`
  with path/renderer/scale/loop; theme/playlist commands answer with a
  clear "scene-mode-only" error.
* `--once` + `--video` renders the first decoded frame offline (no
  display server) with the requested scaling — the deterministic
  pixel-test path (e2e: transparent GIF frame → opaque black;
  `--scale fit` → black bars + placed content).
* No hot reload in media mode (a changed video file is not re-opened
  mid-playback); no audio, ever.

## 5. Measured on this sandbox (2-core, software GL, degraded window)

* 320x180 H.264 30fps loop: decode thread CPU ≈ 2-4% of one core,
  render loop unaffected (video frame grabs are lock-brief).
* 1080p H.264 30fps: decode keeps up with realtime at ~2.5x; the GL
  plane upload is the larger cost on this box's software path.
* **Hardware numbers (1050 Ti class) remain a field-validation item** —
  expected: 1080p30 software decode ~15-25% of one core, 3.1MB/frame
  PCIe upload ≈ 1ms, shader conversion ~free alongside compositing.

## 6. What phase 2+ would add (in value order)

1. **VA-API / NVDEC hardware decode** behind the same layer API
   (EGL dmabuf import for Mesa, CUDA-GL interop for NVIDIA) — removes
   the per-frame CPU decode cost, the only remaining significant CPU
   work in the video path.
2. Frame-accurate seek for a future wallpaper picker (preview scrub).
3. per-monitor playback offsets (currently one shared decoder per
   layer — all monitors show the same frame; deliberate).
4. HDR/10-bit and BT.709 tag handling (current files are overwhelmingly
   8-bit BT.601; the shader constants are ready to parameterize).

## 7. Honest limitations

* Software decode means a 4K120 loop can exceed one core — the layer
  reports decode starvation as a dropped-frame diagnostic rather than
  silently slowing the wallpaper.
* Variable-framerate files play correctly (PTS-driven) but the
  run-ahead queue assumes roughly monotonic timestamps after the
  container's own reorder — as does every player.
* Subtitles/data tracks are ignored; audio is intentionally absent.
