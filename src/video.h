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
 * until the NEXT advance/display call. */
typedef struct {
        const u8 *y, *u, *v;      /* YUV420P planes                     */
        int ls_y, ls_u, ls_v;     /* line sizes (bytes)                */
        u32 w, h;                 /* coded size                        */
        bool ended;               /* stream ended (last frame held)    */
} RVideoFrame;

const RVideoFrame *rev_video_display(RevVideo *v, f64 dt);

/* Diagnostics for the perf/stats path. */
u64 rev_video_dropped(RevVideo *v);

/* CPU-tier fallback: convert the current frame to straight-alpha RGBA8 at
 * (dw x dh) via swscale (BT.601). False when video is unavailable or the
 * frame format is unexpected. */
bool rev_video_to_rgba(const RVideoFrame *f, u32 *dst, u32 dw, u32 dh);

#endif /* REV_VIDEO_H */
