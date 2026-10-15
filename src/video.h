/* ============================================================================
 * Reverie Engine — video.h
 * Video wallpaper decoding: an OPTIONAL multimedia capability, loaded at
 * runtime through dlopen (libavcodec / libavformat / libavutil /
 * libswscale, versioned sonames). The engine builds and runs with zero
 * multimedia dependencies; when the libraries are absent, video layers
 * log one diagnostic and render nothing (see docs/VIDEO.md for the full
 * architecture assessment and the phase-2 hardware-decode plan).
 *
 * Design:
 *   - one decode thread per video layer (the engine's only thread,
 *     contained in video.c) with a bounded run-ahead queue;
 *   - the render loop NEVER blocks on decode — it grabs plane pointers
 *     under a brief mutex, or keeps the previous frame;
 *   - playback clock is the layer's own playhead (delta-time, pause
 *     aware), independent of rendering FPS;
 *   - looping seeks to zero and flushes the codec; EOF holds the last
 *     frame; audio streams are deliberately never opened.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#ifndef REV_VIDEO_H
#define REV_VIDEO_H

#include "rev_types.h"

typedef struct RevVideo RevVideo;

/* Have the multimedia libraries been loadable at all? (logs the reason
 * once when false — callers use this to explain empty video layers) */
bool rev_video_available(void);

/* A byte-range source: either a file path (folder theme) or an in-memory
 * region (packed theme entry). `data` is NOT copied — it must outlive
 * the video (the theme owns it). */
typedef struct {
        const char *path;    /* file source (path wins when non-NULL)   */
        const u8 *data;      /* memory source                          */
        size_t len;
} RVideoSource;

/* Opens (and starts decoding into the queue) a video. Returns NULL with
 * a message when the libraries are missing or the stream is unusable
 * (size caps: 8192x8192). `loop` restarts at EOF. */
RevVideo *rev_video_open(const RVideoSource *src, bool loop,
                         char *err, size_t errcap);
void rev_video_close(RevVideo *v);

/* Advances the playhead (seconds) — pause = dt 0. Selects the frame to
 * display; returns its plane description or NULL while nothing is ready
 * (the caller keeps showing whatever it had). Plane pointers stay valid
 * until the NEXT advance/display call.
 *
 * Two frame families (set by the decoder, checked via `rgb`):
 *   rgb == false  YUV420P planes (y/u/v + line sizes) — the common video
 *                 case; GPU tiers upload them as R8 textures.
 *   rgb == true   packed straight-alpha RGBA8 in `rgba` (line size
 *                 ls_rgba == w*4, tight rows) — RGB-family sources
 *                 (animated GIF/PAL8, PNG-frame streams, RGBA MOVs),
 *                 normalized by the decode thread. Byte order is the
 *                 engine's native 0xAABBGGRR packing (R,G,B,A in memory
 *                 on little-endian), identical to RFb/cmd.px pixels. */
typedef struct {
        const u8 *y, *u, *v;      /* YUV420P planes (when !rgb)         */
        int ls_y, ls_u, ls_v;     /* line sizes (bytes)                 */
        bool rgb;                 /* true: rgba[] is the frame          */
        const u8 *rgba;
        int ls_rgba;              /* bytes per line (== w*4, tight)     */
        u32 w, h;                 /* coded size                         */
        bool ended;               /* stream ended (last frame held)     */
} RVideoFrame;

const RVideoFrame *rev_video_display(RevVideo *v, f64 dt);

/* Diagnostics for the perf/stats path. */
u64 rev_video_dropped(RevVideo *v);

/* CPU-tier conversion: any frame family -> straight-alpha RGBA8 at
 * (dw x dh) through swscale (BT.601 for YUV sources; RGBA sources keep
 * their alpha). False when video is unavailable or the frame format is
 * unexpected. */
bool rev_video_to_rgba(const RVideoFrame *f, u32 *dst, u32 dw, u32 dh);

/* The same, into a caller-placed rectangle: `dst` may sit at an offset
 * inside a larger buffer whose row length is `dst_stride` u32 ELEMENTS
 * (dst_stride >= dw). One conversion, no intermediate blit.
 *
 * PADDING CONTRACT: sws may write up to ONE EXTRA OUTPUT ROW past the
 * last (observed 8 u32 past the final row on sub-rect SIMD paths —
 * FFmpeg-internal, varies by build). Callers of _at/_sub must provide
 * dst_stride * (dh + 1) + 16 u32 of capacity. rev_video_to_rgba() wraps
 * this with its own padding and keeps the exact dw*dh contract. */
bool rev_video_to_rgba_at(const RVideoFrame *f, u32 *dst, u32 dst_stride,
                          u32 dw, u32 dh);

/* Scales the SOURCE SUB-RECT [sx, sy, sw x sh] of the frame into dw x dh
 * at dst (row length dst_stride u32 elements, same padding contract as
 * _at). This is how cover-crop stays bounded on the CPU tier: instead
 * of scaling 100x10000 to 1920x192000 and blitting the visible strip,
 * the visible source region is scaled directly into the viewport.
 * Chroma planes are offset by sx/2, sy/2 (rounded down). */
bool rev_video_to_rgba_sub(const RVideoFrame *f, u32 sx, u32 sy,
                           u32 sw, u32 sh, u32 *dst, u32 dst_stride,
                           u32 dw, u32 dh);

/* Playback pacing rate: the stream's average frame rate (Hz), falling
 * back to the codec's declared rate, then to 30 — the media-mode render
 * loop paces to this so uploads happen at source speed, not render
 * speed. Set once at open; the loop-reopen path re-reads the same file. */
f64 rev_video_fps(const RevVideo *v);

#endif /* REV_VIDEO_H */
