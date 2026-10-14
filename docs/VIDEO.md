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
* **Looping** via seek-to-zero + codec flush; clean EOF holds the last
  frame. Playback timing is driven by the layer's own playhead
  (delta-time, pause-aware) — rendering FPS does not change playback
  speed.
* **Audio: not opened.** The video stream is selected explicitly; audio
  tracks are ignored (a wallpaper has no business making sound).
* Scene language: `layer v { type = video; src = "videos/loop.mp4";
  loop = true; fit = fill; paused = false }` — a normal layer that
  composites with blend/opacity alongside everything else.
* **GL tier**: Y/U/V planes uploaded as R8 textures, BT.601
  limited-range YUV→RGB in a fragment shader during the standard
  composite pass (placement/blend/opacity shared with image layers).
* **CPU tier**: `sws_scale` straight into the placed rectangle —
  functional fallback, identical visual result.
* Resource safety: videos > 8192×8192 or broken streams are refused with
  a message; decode errors drop the layer (one diagnostic), never the
  engine; shutdown joins the decode thread.

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
