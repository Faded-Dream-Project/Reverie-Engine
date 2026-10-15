/* ============================================================================
 * Reverie Engine — video.c
 * Optional FFmpeg-based video decoding, entirely through dlopen.
 * See docs/VIDEO.md for the architecture assessment.
 *
 * Build-time: compiled only when FFmpeg headers are available (system
 * include paths, or the vendored contrib/ffmpeg-headers). The structs are
 * the REAL layouts from those headers; the LIBRARIES are still loaded at
 * runtime with dlopen, and their ABI major versions are checked against
 * the headers before any struct is touched — a version mismatch refuses
 * the video layer with an explanation instead of corrupting memory.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "video.h"

#include <dlfcn.h>
#include <pthread.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>

/* ---- the dlopen'd function table (signatures from the headers) ------------- */

typedef struct {
        void *avcodec, *avformat, *avutil, *swscale;

        unsigned (*avcodec_version)(void);
        unsigned (*avformat_version)(void);
        unsigned (*avutil_version)(void);

        AVFormatContext *(*avformat_alloc_context)(void);
        AVIOContext *(*avio_alloc_context)(unsigned char *buffer, int buffer_size,
                                           int write_flag, void *opaque,
                                           int (*read_packet)(void *, unsigned char *, int),
                                           int (*write_packet)(void *, unsigned char *, int),
                                           int64_t (*seek)(void *, int64_t, int));
        int (*avformat_open_input)(AVFormatContext **ps, const char *url,
                                   const AVInputFormat *fmt, AVDictionary **options);
        void (*avformat_close_input)(AVFormatContext **s);
        int (*avformat_find_stream_info)(AVFormatContext *ic, AVDictionary **options);
        int (*av_find_best_stream)(AVFormatContext *ic, enum AVMediaType type,
                                   int wanted_stream_nb, int related_stream,
                                   const AVCodec **decoder_ret, int flags);
        int (*av_read_frame)(AVFormatContext *s, AVPacket *pkt);
        int (*av_seek_frame)(AVFormatContext *s, int stream_index, int64_t timestamp,
                             int flags);

        const AVCodec *(*avcodec_find_decoder)(enum AVCodecID id);
        AVCodecContext *(*avcodec_alloc_context3)(const AVCodec *codec);
        int (*avcodec_parameters_to_context)(AVCodecContext *codec_ctx,
                                             const AVCodecParameters *par);
        int (*avcodec_open2)(AVCodecContext *avctx, const AVCodec *codec,
                             AVDictionary **options);
        void (*avcodec_free_context)(AVCodecContext **avctx);
        int (*avcodec_send_packet)(AVCodecContext *avctx, const AVPacket *avpkt);
        int (*avcodec_receive_frame)(AVCodecContext *avctx, AVFrame *frame);
        void (*avcodec_flush_buffers)(AVCodecContext *avctx);

        AVPacket *(*av_packet_alloc)(void);
        void (*av_packet_free)(AVPacket **pkt);
        void (*av_packet_unref)(AVPacket *pkt);

        AVFrame *(*av_frame_alloc)(void);
        void (*av_frame_free)(AVFrame **frame);
        int (*av_frame_ref)(AVFrame *dst, const AVFrame *src);
        void (*av_frame_unref)(AVFrame *frame);

        void *(*av_malloc)(size_t size);
        void (*av_free)(void *ptr);
        AVBufferRef *(*av_buffer_alloc)(int size);

        struct SwsContext *(*sws_getContext)(int srcW, int srcH, enum AVPixelFormat srcFormat,
                                             int dstW, int dstH, enum AVPixelFormat dstFormat,
                                             int flags, const SwsFilter *srcFilter,
                                             const SwsFilter *dstFilter, const double *param);
        void (*sws_freeContext)(struct SwsContext *sws);
        int (*sws_scale)(struct SwsContext *c, const uint8_t *const srcSlice[],
                         const int srcStride[], int srcSliceY, int srcSliceH,
                         uint8_t *const dst[], const int dstStride[]);
} FF;

static FF g_ff;
static bool g_ff_tried;
static bool g_ff_ok;

#define LOAD(lib, fn) do { \
        *(void **)&g_ff.fn = dlsym(g_ff.lib, #fn); \
        if (!g_ff.fn) { \
                LOGW("video: %s missing symbol %s", #lib, #fn); \
                return false; \
        } \
} while (0)

static void *try_dlopen(const char *const *names, int n)
{
        for (int i = 0; i < n; i++) {
                void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
                if (h)
                        return h;
        }
        return NULL;
}

static bool ff_load(void)
{
        if (g_ff_tried)
                return g_ff_ok;
        g_ff_tried = true;

        /* versioned sonames, newest first — the ABI check below decides */
        static const char *const avcodec_n[] = {
                "libavcodec.so." AV_STRINGIFY(LIBAVCODEC_VERSION_MAJOR),
                "libavcodec.so.61", "libavcodec.so.60", "libavcodec.so.59",
                "libavcodec.so.58", "libavcodec.so"
        };
        static const char *const avformat_n[] = {
                "libavformat.so." AV_STRINGIFY(LIBAVFORMAT_VERSION_MAJOR),
                "libavformat.so.61", "libavformat.so.60", "libavformat.so.59",
                "libavformat.so.58", "libavformat.so"
        };
        static const char *const avutil_n[] = {
                "libavutil.so." AV_STRINGIFY(LIBAVUTIL_VERSION_MAJOR),
                "libavutil.so.59", "libavutil.so.58", "libavutil.so.57",
                "libavutil.so.56", "libavutil.so"
        };
        static const char *const swscale_n[] = {
                "libswscale.so." AV_STRINGIFY(LIBSWSCALE_VERSION_MAJOR),
                "libswscale.so.8", "libswscale.so.7", "libswscale.so.6",
                "libswscale.so.5", "libswscale.so"
        };
        g_ff.avcodec = try_dlopen(avcodec_n, 6);
        g_ff.avformat = try_dlopen(avformat_n, 6);
        g_ff.avutil = try_dlopen(avutil_n, 6);
        g_ff.swscale = try_dlopen(swscale_n, 6);
        if (!g_ff.avcodec || !g_ff.avformat || !g_ff.avutil || !g_ff.swscale) {
                LOGI("video: multimedia libraries not present (libavcodec/"
                     "libavformat/libavutil/libswscale) — video wallpapers "
                     "unavailable, everything else unaffected");
                return false;
        }

        /* function table */
        LOAD(avutil, avutil_version);
        LOAD(avcodec, avcodec_version);
        LOAD(avformat, avformat_version);
        LOAD(avutil, av_malloc);
        LOAD(avutil, av_free);
        LOAD(avutil, av_buffer_alloc);
        LOAD(avformat, avformat_alloc_context);
        LOAD(avformat, avio_alloc_context);
        LOAD(avformat, avformat_open_input);
        LOAD(avformat, avformat_close_input);
        LOAD(avformat, avformat_find_stream_info);
        LOAD(avformat, av_find_best_stream);
        LOAD(avformat, av_read_frame);
        LOAD(avformat, av_seek_frame);
        LOAD(avcodec, avcodec_find_decoder);
        LOAD(avcodec, avcodec_alloc_context3);
        LOAD(avcodec, avcodec_parameters_to_context);
        LOAD(avcodec, avcodec_open2);
        LOAD(avcodec, avcodec_free_context);
        LOAD(avcodec, avcodec_send_packet);
        LOAD(avcodec, avcodec_receive_frame);
        LOAD(avcodec, avcodec_flush_buffers);
        LOAD(avcodec, av_packet_alloc);
        LOAD(avcodec, av_packet_free);
        LOAD(avcodec, av_packet_unref);
        LOAD(avutil, av_frame_alloc);
        LOAD(avutil, av_frame_free);
        LOAD(avutil, av_frame_ref);
        LOAD(avutil, av_frame_unref);
        LOAD(swscale, sws_getContext);
        LOAD(swscale, sws_freeContext);
        LOAD(swscale, sws_scale);

        /* ABI guard: the structs this file touches were laid out by the
         * headers it was BUILT against; the loaded libraries must match
         * those majors, or every field access is undefined. Mismatch ->
         * honest refusal (rebuild against your system's headers). */
        unsigned cac = g_ff.avcodec_version() >> 16;
        unsigned caf = g_ff.avformat_version() >> 16;
        unsigned cau = g_ff.avutil_version() >> 16;
        unsigned (*swver)(void);
        *(void **)&swver = dlsym(g_ff.swscale, "swscale_version");
        unsigned csw = swver ? swver() >> 16 : 0;
        if (cac != (unsigned)LIBAVCODEC_VERSION_MAJOR ||
            caf != (unsigned)LIBAVFORMAT_VERSION_MAJOR ||
            cau != (unsigned)LIBAVUTIL_VERSION_MAJOR ||
            csw != (unsigned)LIBSWSCALE_VERSION_MAJOR) {
                LOGW("video: FFmpeg ABI mismatch (loaded avcodec/avformat/avutil/"
                     "swscale majors %u/%u/%u/%u, built for %u/%u/%u/%u) — video "
                     "wallpapers disabled in this build; rebuild against your "
                     "system's FFmpeg headers to enable them",
                     cac, caf, cau, csw,
                     LIBAVCODEC_VERSION_MAJOR, LIBAVFORMAT_VERSION_MAJOR,
                     LIBAVUTIL_VERSION_MAJOR, LIBSWSCALE_VERSION_MAJOR);
                memset(&g_ff, 0, sizeof g_ff);
                return false;
        }
        g_ff_ok = true;
        LOGI("video: FFmpeg %u.%u loaded (software decode, GPU color convert)",
             cac, g_ff.avutil_version() & 0xff);
        return true;
}

bool rev_video_available(void)
{
        return ff_load();
}

/* ---- decoder state ----------------------------------------------------------- */

#define VQ_MAX 6              /* run-ahead frames in the queue            */

struct RevVideo {
        RVideoSource src;
        bool loop;

        pthread_t thread;
        bool thread_started;
        pthread_mutex_t mu;
        pthread_cond_t cv;

        /* producer state (decode thread only) */
        AVFormatContext *fmt;
        AVCodecContext *cdc;
        int vstream;
        AVPacket *pkt;
        /* custom AVIO (memory source): fmt->pb is OURS to free —
         * avformat_close_input leaves custom-IO contexts alone, and the
         * 2026-10 leak audit showed the 64KB buffer + box + context
         * leaked on every memory-source open (packed themes) */
        AVIOContext *avio;
        void *avio_box;

        /* shared (lock-guarded) */
        AVFrame *q[VQ_MAX];
        int qn;
        int qcap;           /* effective queue depth: VQ_MAX for plane
                             * sources, memory-bounded for RGB sources  */
        AVFrame *cur;         /* frame being displayed (ref'd)            */
        f64 cur_pts;
        f64 cur_end;          /* pts + duration of `cur` — the wrap check
                               * needs the frame's END, not its start:
                               * wrapping when the queue merely drains
                               * truncates the last frame to one tick   */
        bool dead;            /* irrecoverable error / shutdown           */
        bool ended;           /* stream ended (non-loop)                  */
        bool wrap_pending;    /* loop mode: decoder hit EOF and waits for
                               * the consumer to drain the queue and wrap
                               * the playhead (consumer-driven wrap — a
                               * decoder-side reset fast-forwards or, on
                               * files shorter than the queue, resets the
                               * playhead before the frames ever show)    */
        u64 dropped;

        /* RGB normalization (decode thread only): non-YUV420P frames are
         * converted to packed RGBA right where they decode, so the render
         * thread only ever grabs pointers. The sws context is keyed on
         * srcW/srcH/srcFormat; GIFs may change palette mid-stream without
         * changing the format key (the palette travels per-frame). */
        struct SwsContext *rgb_sws;
        int rgb_sws_w, rgb_sws_h, rgb_sws_fmt;

        /* display side (render thread) */
        f64 playhead;
        f64 fps;              /* pacing rate, set once at open          */
        RVideoFrame disp;
};

/* ---- AVIO over an in-memory pack entry --------------------------------------- */

typedef struct {
        const u8 *data;
        size_t len;
        int64_t pos;
} AVIOBox;

static int avio_read_box(void *opaque, uint8_t *buf, int size)
{
        AVIOBox *b = (AVIOBox *)opaque;
        if (b->pos >= (int64_t)b->len)
                return AVERROR_EOF;
        int64_t avail = (int64_t)b->len - b->pos;
        int n = size < avail ? size : (int)avail;
        memcpy(buf, b->data + b->pos, (size_t)n);
        b->pos += n;
        return n;
}

static int64_t avio_seek_box(void *opaque, int64_t off, int whence)
{
        AVIOBox *b = (AVIOBox *)opaque;
        int64_t np;
        switch (whence) {
        case SEEK_CUR: np = b->pos + off; break;
        case SEEK_END: np = (int64_t)b->len + off; break;
        case AVSEEK_SIZE: return (int64_t)b->len;
        default: np = off; break;                    /* SEEK_SET */
        }
        if (np < 0)
                np = 0;
        if (np > (int64_t)b->len)
                np = (int64_t)b->len;
        b->pos = np;
        return np;
}

/* ---- the decode thread --------------------------------------------------------- */

static f64 pts_seconds(int64_t pts, AVRational tb)
{
        if (pts == AV_NOPTS_VALUE || !tb.den)
                return -1e18;
        return (f64)pts * (f64)tb.num / (f64)tb.den;
}

/* Non-YUV420P frames (animated GIF/PAL8, RGBA MOVs, PNG streams...) are
 * normalized HERE, on the decode thread, into a synthetic AVFrame that
 * owns one refcounted RGBA buffer:
 *   - format AV_PIX_FMT_RGBA — byte order R,G,B,A, which is exactly the
 *     engine's native 0xAABBGGRR u32 packing in little-endian memory;
 *   - data[0] tight rows (linesize == w*4) so the buffer can serve as a
 *     plain cmd.px raster without any re-pack;
 *   - pts/duration copied from the source frame.
 * YUV420P (and the full-range YUVJ420P alias) passes through unchanged —
 * the GPU tiers' plane-upload path keeps its 2.7x bandwidth advantage.
 * Returns the replacement frame (the caller frees the original). */
static AVFrame *normalize_rgb(RevVideo *v, AVFrame *f)
{
        if (f->format == AV_PIX_FMT_YUV420P ||
            f->format == AV_PIX_FMT_YUVJ420P)
                return f;

        if (v->rgb_sws &&
            (v->rgb_sws_w != f->width || v->rgb_sws_h != f->height ||
             v->rgb_sws_fmt != f->format)) {
                g_ff.sws_freeContext(v->rgb_sws);
                v->rgb_sws = NULL;
        }
        if (!v->rgb_sws) {
                v->rgb_sws = g_ff.sws_getContext(f->width, f->height,
                                                 (enum AVPixelFormat)f->format,
                                                 f->width, f->height,
                                                 AV_PIX_FMT_RGBA,
                                                 SWS_BILINEAR, NULL, NULL, NULL);
                v->rgb_sws_w = f->width;
                v->rgb_sws_h = f->height;
                v->rgb_sws_fmt = f->format;
                if (!v->rgb_sws)
                        return f;    /* cannot convert: show planes (best effort) */
        }

        AVFrame *o = g_ff.av_frame_alloc();
        if (!o)
                return f;
        const int ls = f->width * 4;
        o->buf[0] = g_ff.av_buffer_alloc((size_t)ls * f->height);
        if (!o->buf[0]) {
                g_ff.av_frame_free(&o);
                return f;
        }
        o->data[0] = o->buf[0]->data;
        o->linesize[0] = ls;
        o->format = AV_PIX_FMT_RGBA;
        o->width = f->width;
        o->height = f->height;
        o->pts = f->pts;

        /* PAL8 carries its palette in data[1]; every other format is a
         * plain plane list. sws reads the palette per call, so mid-stream
         * palette changes (GIF local color tables) need no re-keying. */
        const uint8_t *src[4] = { f->data[0], f->data[1], NULL, NULL };
        const int src_ls[4] = { f->linesize[0], f->linesize[1], 0, 0 };
        uint8_t *dst[1] = { o->data[0] };
        const int dst_ls[1] = { ls };
        if (g_ff.sws_scale(v->rgb_sws, src, src_ls, 0, f->height,
                           dst, dst_ls) != f->height) {
                g_ff.av_frame_free(&o);
                return f;
        }
        g_ff.av_frame_free(&f);
        return o;
}

/* Reopens the demuxer + decoder from byte zero (decode thread). Used by
 * looping when the demuxer has no seek support — the animated-GIF
 * demuxer is the notable member of that family. Swaps the new contexts
 * in under the lock (rev_video_display reads fmt->streams[] under the
 * same lock) and resets the playback state. Returns false when even the
 * reopen fails (treated as end-of-stream). */
static bool reopen_input(RevVideo *v)
{
        AVFormatContext *fmt = NULL;
        AVCodecContext *cdc = NULL;
        AVIOContext *avio = NULL;
        AVIOBox *box = NULL;
        int vstream = -1;

        fmt = g_ff.avformat_alloc_context();
        if (!fmt)
                return false;
        if (v->src.path) {
                if (g_ff.avformat_open_input(&fmt, v->src.path, NULL, NULL) < 0)
                        goto fail;
        } else {
                box = (AVIOBox *)calloc(1, sizeof *box);
                unsigned char *buf = (unsigned char *)g_ff.av_malloc(64 * 1024);
                if (!box || !buf)
                        goto fail;
                box->data = v->src.data;
                box->len = v->src.len;
                avio = g_ff.avio_alloc_context(buf, 64 * 1024, 0, box,
                                               avio_read_box, NULL, avio_seek_box);
                if (!avio) {
                        g_ff.av_free(buf);
                        goto fail;
                }
                fmt->pb = avio;
                if (g_ff.avformat_open_input(&fmt, "packed.mp4", NULL, NULL) < 0)
                        goto fail;
        }
        if (g_ff.avformat_find_stream_info(fmt, NULL) < 0)
                goto fail;
        {
                const AVCodec *dec = NULL;
                vstream = g_ff.av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO,
                                                   -1, -1, &dec, 0);
                if (vstream < 0 || !dec)
                        goto fail;
                cdc = g_ff.avcodec_alloc_context3(dec);
                if (!cdc)
                        goto fail;
                cdc->thread_count = 1;
                if (g_ff.avcodec_parameters_to_context(
                            cdc, fmt->streams[vstream]->codecpar) < 0 ||
                    g_ff.avcodec_open2(cdc, dec, NULL) < 0)
                        goto fail;
        }

        /* swap under the lock: display reads the old stream's time base
         * through v->fmt; frames it still holds are refcounted and safe.
         * (The queue is empty and the playhead already wrapped — the
         * consumer-driven protocol guarantees both before we get here.) */
        pthread_mutex_lock(&v->mu);
        AVFormatContext *of = v->fmt;
        AVCodecContext *oc = v->cdc;
        AVIOContext *oa = v->avio;
        AVIOBox *ob = v->avio_box;
        v->fmt = fmt;
        v->cdc = cdc;
        v->vstream = vstream;
        v->avio = avio;
        v->avio_box = box;
        pthread_cond_broadcast(&v->cv);
        pthread_mutex_unlock(&v->mu);

        /* free the replaced contexts OUTSIDE the lock */
        if (oc)
                g_ff.avcodec_free_context(&oc);
        if (of)
                g_ff.avformat_close_input(&of);
        if (oa) {
                if (oa->buffer)
                        g_ff.av_free(oa->buffer);
                g_ff.av_free(oa);
        }
        free(ob);
        return true;

fail:
        if (cdc)
                g_ff.avcodec_free_context(&cdc);
        if (fmt)
                g_ff.avformat_close_input(&fmt);
        if (avio) {
                if (avio->buffer)
                        g_ff.av_free(avio->buffer);
                g_ff.av_free(avio);
        }
        free(box);
        return false;
}

static void *decode_thread(void *arg)
{
        RevVideo *v = (RevVideo *)arg;

        for (;;) {
                pthread_mutex_lock(&v->mu);
                while (!v->dead && v->qn == v->qcap)
                        pthread_cond_wait(&v->cv, &v->mu);
                bool dead = v->dead;
                pthread_mutex_unlock(&v->mu);
                if (dead)
                        break;

                int rc = g_ff.av_read_frame(v->fmt, v->pkt);
                if (rc < 0) {
                        /* EOF or read error: loop or finish.
                         *
                         * Looping is CONSUMER-DRIVEN: mark wrap_pending
                         * and sleep until the render side drains the
                         * queue and resets the playhead itself. A
                         * decoder-side playhead reset races the consumer
                         * — on files shorter than the queue depth (a
                         * 2-frame GIF) the decoder wraps so fast the
                         * playhead never advances past frame 0, and on
                         * longer files the still-queued tail of cycle N
                         * plays during cycle N+1. Sleeping here also
                         * means a paused/hidden daemon stops decoding
                         * entirely instead of spinning through EOF. */
                        bool looped = false;
                        if (v->loop) {
                                pthread_mutex_lock(&v->mu);
                                v->wrap_pending = true;
                                pthread_cond_broadcast(&v->cv);
                                while (v->wrap_pending && !v->dead)
                                        pthread_cond_wait(&v->cv, &v->mu);
                                const bool dead2 = v->dead;
                                pthread_mutex_unlock(&v->mu);
                                if (!dead2) {
                                        if (g_ff.av_seek_frame(v->fmt, -1, 0,
                                                               AVSEEK_FLAG_BACKWARD) >= 0) {
                                                g_ff.avcodec_flush_buffers(v->cdc);
                                                looped = true;
                                        } else {
                                                /* no seek support in this
                                                 * demuxer (animated GIF):
                                                 * reopen from zero */
                                                looped = reopen_input(v);
                                        }
                                }
                        }
                        if (looped) {
                                g_ff.av_packet_unref(v->pkt);
                                continue;
                        }
                        pthread_mutex_lock(&v->mu);
                        v->ended = true;
                        pthread_cond_broadcast(&v->cv);
                        pthread_mutex_unlock(&v->mu);
                        break;
                }

                if (v->pkt->stream_index != v->vstream) {
                        g_ff.av_packet_unref(v->pkt);
                        continue;
                }
                g_ff.avcodec_send_packet(v->cdc, v->pkt);
                g_ff.av_packet_unref(v->pkt);

                for (;;) {
                        AVFrame *f = g_ff.av_frame_alloc();
                        if (!f)
                                break;
                        if (g_ff.avcodec_receive_frame(v->cdc, f) < 0) {
                                g_ff.av_frame_free(&f);
                                break;
                        }
                        f = normalize_rgb(v, f);
                        pthread_mutex_lock(&v->mu);
                        if (v->dead) {
                                g_ff.av_frame_free(&f);
                                pthread_mutex_unlock(&v->mu);
                                break;
                        }
                        if (v->qn == v->qcap) {
                                /* consumer fell behind: drop the oldest */
                                g_ff.av_frame_free(&v->q[0]);
                                memmove(&v->q[0], &v->q[1],
                                        sizeof(AVFrame *) * (size_t)(v->qcap - 1));
                                v->q[v->qcap - 1] = f;
                                v->dropped++;
                        } else {
                                v->q[v->qn++] = f;
                        }
                        pthread_cond_broadcast(&v->cv);
                        pthread_mutex_unlock(&v->mu);
                }
        }
        return NULL;
}

/* ---- open / close ---------------------------------------------------------------- */

RevVideo *rev_video_open(const RVideoSource *src, bool loop,
                         char *err, size_t errcap)
{
        if (!src || (!src->path && !src->data)) {
                snprintf(err, errcap, "video: empty source");
                return NULL;
        }
        if (!ff_load()) {
                /* one refusal contract for every disable reason (libraries
                 * missing, symbols missing, ABI mismatch) and for the
                 * no-headers stub build: the "no video support" anchor */
                snprintf(err, errcap,
                         "video: no video support — FFmpeg libraries unusable "
                         "on this system (missing or ABI-mismatched; see the "
                         "log for the reason)");
                return NULL;
        }

        RevVideo *v = (RevVideo *)calloc(1, sizeof(RevVideo));
        if (!v) {
                snprintf(err, errcap, "out of memory");
                return NULL;
        }
        v->src = *src;
        v->loop = loop;

        AVIOBox *box = NULL;
        bool ok = false;

        v->fmt = g_ff.avformat_alloc_context();
        if (!v->fmt) {
                snprintf(err, errcap, "video: format context alloc failed");
                goto out;
        }

        if (src->path) {
                if (g_ff.avformat_open_input(&v->fmt, src->path, NULL, NULL) < 0) {
                        snprintf(err, errcap, "video: cannot open '%s'", src->path);
                        goto out;
                }
        } else {
                box = (AVIOBox *)calloc(1, sizeof(AVIOBox));
                unsigned char *buf = (unsigned char *)g_ff.av_malloc(64 * 1024);
                if (!box || !buf) {
                        snprintf(err, errcap, "video: out of memory (AVIO)");
                        goto out;
                }
                box->data = src->data;
                box->len = src->len;
                AVIOContext *avio = g_ff.avio_alloc_context(buf, 64 * 1024, 0, box,
                                                            avio_read_box, NULL,
                                                            avio_seek_box);
                if (!avio) {
                        snprintf(err, errcap, "video: AVIO context failed");
                        goto out;
                }
                v->fmt->pb = avio;
                v->avio = avio;
                v->avio_box = box;
                box = NULL;    /* ownership moved to RevVideo */
                if (g_ff.avformat_open_input(&v->fmt, "packed.mp4", NULL, NULL) < 0) {
                        snprintf(err, errcap, "video: cannot open packed video");
                        goto out;
                }
        }

        if (g_ff.avformat_find_stream_info(v->fmt, NULL) < 0) {
                snprintf(err, errcap, "video: no stream info (corrupt file?)");
                goto out;
        }
        const AVCodec *dec = NULL;
        v->vstream = g_ff.av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO,
                                              -1, -1, &dec, 0);
        if (v->vstream < 0 || !dec) {
                snprintf(err, errcap, "video: no decodable video stream");
                goto out;
        }
        v->cdc = g_ff.avcodec_alloc_context3(dec);
        if (!v->cdc) {
                snprintf(err, errcap, "video: codec context alloc failed");
                goto out;
        }
        AVStream *st = v->fmt->streams[v->vstream];
        /* single-threaded codec: we provide the thread (see video.h); with
         * FFmpeg's own frame-threading the decoder's delayed frames at EOF
         * and our queue advance interleave badly on some builds */
        v->cdc->thread_count = 1;
        if (g_ff.avcodec_parameters_to_context(v->cdc, st->codecpar) < 0 ||
            g_ff.avcodec_open2(v->cdc, dec, NULL) < 0) {
                snprintf(err, errcap, "video: codec open failed");
                goto out;
        }
        if (v->cdc->width <= 0 || v->cdc->height <= 0 ||
            v->cdc->width > 8192 || v->cdc->height > 8192) {
                snprintf(err, errcap, "video: unsupported dimensions %dx%d",
                         v->cdc->width, v->cdc->height);
                goto out;
        }

        /* queue depth: plane frames are refcounted (cheap), but RGB-family
         * sources hold full RGBA copies — bound the run-ahead memory so a
         * 4K RGBA stream cannot queue 200 MB. 48 MB / frame bytes, clamped
         * to [2, VQ_MAX]. */
        v->qcap = VQ_MAX;
        const bool rgb_family =
                v->cdc->pix_fmt != AV_PIX_FMT_YUV420P &&
                v->cdc->pix_fmt != AV_PIX_FMT_YUVJ420P;
        if (rgb_family) {
                size_t bytes = (size_t)v->cdc->width * v->cdc->height * 4 + 4096;
                int by_mem = (int)(48u << 20) / (int)(bytes > INT_MAX ? INT_MAX : bytes);
                v->qcap = by_mem < 2 ? 2 : (by_mem > VQ_MAX ? VQ_MAX : by_mem);
        }

        /* pacing rate: the stream's average frame rate, else the codec's
         * declared rate, else 30 — set before the thread spawns, never
         * touched again (the loop-reopen path re-reads the same file) */
        {
                AVStream *st0 = v->fmt->streams[v->vstream];
                AVRational fr = st0->avg_frame_rate;
                f64 fps = (fr.num && fr.den) ? (f64)fr.num / (f64)fr.den : 0.0;
                if (fps <= 0.0 || fps > 240.0) {
                        fr = v->cdc->framerate;
                        fps = (fr.num && fr.den) ? (f64)fr.num / (f64)fr.den : 0.0;
                }
                if (fps <= 0.0 || fps > 240.0)
                        fps = 30.0;
                v->fps = fps;
        }

        v->pkt = g_ff.av_packet_alloc();
        if (!v->pkt) {
                snprintf(err, errcap, "video: packet alloc failed");
                goto out;
        }

        if (pthread_mutex_init(&v->mu, NULL) != 0 ||
            pthread_cond_init(&v->cv, NULL) != 0) {
                snprintf(err, errcap, "video: sync primitives failed");
                goto out;
        }
        if (pthread_create(&v->thread, NULL, decode_thread, v) != 0) {
                snprintf(err, errcap, "video: decode thread spawn failed");
                pthread_cond_destroy(&v->cv);
                pthread_mutex_destroy(&v->mu);
                goto out;
        }
        v->thread_started = true;
        ok = true;
        LOGI("video: %dx%d (%s, %s, queue %d), loop=%s",
             v->cdc->width, v->cdc->height, dec->name,
             rgb_family ? "rgb frames" : "yuv420p planes", v->qcap,
             loop ? "yes" : "no");

out:
        if (!ok) {
                if (v->fmt)
                        g_ff.avformat_close_input(&v->fmt);
                if (v->avio) {
                        if (v->avio->buffer)
                                g_ff.av_free(v->avio->buffer);
                        g_ff.av_free(v->avio);
                        v->avio = NULL;
                }
                free(v->avio_box);
                v->avio_box = NULL;
                if (v->cdc)
                        g_ff.avcodec_free_context(&v->cdc);
                if (v->pkt)
                        g_ff.av_packet_free(&v->pkt);
                free(box);
                free(v);
                return NULL;
        }
        return v;
}

void rev_video_close(RevVideo *v)
{
        if (!v)
                return;
        if (v->thread_started) {
                pthread_mutex_lock(&v->mu);
                v->dead = true;
                pthread_cond_broadcast(&v->cv);
                pthread_mutex_unlock(&v->mu);
                pthread_join(v->thread, NULL);
        }
        for (int i = 0; i < VQ_MAX; i++) {
                if (v->q[i]) {
                        g_ff.av_frame_free(&v->q[i]);
                        v->q[i] = NULL;
                }
        }
        if (v->cur)
                g_ff.av_frame_free(&v->cur);
        if (v->pkt)
                g_ff.av_packet_free(&v->pkt);
        if (v->cdc)
                g_ff.avcodec_free_context(&v->cdc);
        if (v->fmt)
                g_ff.avformat_close_input(&v->fmt);
        if (v->rgb_sws)
                g_ff.sws_freeContext(v->rgb_sws);
        if (v->avio) {
                /* custom IO: the format context never owned fmt->pb */
                if (v->avio->buffer)
                        g_ff.av_free(v->avio->buffer);
                g_ff.av_free(v->avio);
                v->avio = NULL;
        }
        free(v->avio_box);
        v->avio_box = NULL;
        pthread_cond_destroy(&v->cv);
        pthread_mutex_destroy(&v->mu);
        free(v);
}

/* ---- display selection ------------------------------------------------------------- */

const RVideoFrame *rev_video_display(RevVideo *v, f64 dt)
{
        if (!v)
                return NULL;
        v->playhead += dt > 0 ? dt : 0;
        if (v->playhead > 3600.0 * 12.0)
                v->playhead = 0;

        pthread_mutex_lock(&v->mu);
        AVStream *st = v->fmt->streams[v->vstream];
        int before = v->qn;

        while (v->qn > 0) {
                AVFrame *f = v->q[0];
                f64 pts = pts_seconds(f->pts, st->time_base);
                if (pts < -1e17) {
                        /* no timestamp: advance monotonically from the last */
                        pts = v->cur_pts < -1e17 ? 0 : v->cur_pts + 1.0 / 30.0;
                }
                if (pts <= v->playhead + 0.001) {
                        if (!v->cur)
                                v->cur = g_ff.av_frame_alloc();
                        if (v->cur) {
                                g_ff.av_frame_unref(v->cur);
                                g_ff.av_frame_ref(v->cur, f);
                                v->cur_pts = pts;
                                /* the frame's END drives the loop wrap.
                                 * The next queued frame's pts is the exact
                                 * end (variable frame rates included); with
                                 * an empty queue fall back to one average
                                 * frame period. No duration metadata is
                                 * read — its field name moved across FFmpeg
                                 * majors (pkt_duration -> duration), and
                                 * distro header sets disagree about which
                                 * exists at which version. */
                                {
                                        f64 end = pts +
                                                (v->fps > 0.0 ? 1.0 / v->fps
                                                              : 1.0 / 30.0);
                                        if (v->qn >= 2) {
                                                f64 npts = pts_seconds(
                                                        v->q[1]->pts,
                                                        st->time_base);
                                                if (npts < -1e17)
                                                        npts = pts + 1.0 / 30.0;
                                                if (npts > end)
                                                        end = npts;
                                        }
                                        v->cur_end = end;
                                }
                                if (v->cur->format == AV_PIX_FMT_RGBA) {
                                        /* normalized RGB-family frame:
                                         * packed straight-alpha RGBA8,
                                         * tight rows, engine byte order */
                                        v->disp.rgb = true;
                                        v->disp.rgba = v->cur->data[0];
                                        v->disp.ls_rgba = v->cur->linesize[0];
                                        v->disp.y = v->disp.u = v->disp.v = NULL;
                                        v->disp.ls_y = v->disp.ls_u = v->disp.ls_v = 0;
                                } else {
                                        v->disp.rgb = false;
                                        v->disp.rgba = NULL;
                                        v->disp.ls_rgba = 0;
                                        v->disp.y = v->cur->data[0];
                                        v->disp.u = v->cur->data[1];
                                        v->disp.v = v->cur->data[2];
                                        v->disp.ls_y = v->cur->linesize[0];
                                        v->disp.ls_u = v->cur->linesize[1];
                                        v->disp.ls_v = v->cur->linesize[2];
                                }
                                v->disp.w = (u32)v->cur->width;
                                v->disp.h = (u32)v->cur->height;
                                v->disp.ended = v->ended && v->qn == 0;
                        }
                        g_ff.av_frame_free(&f);
                        memmove(&v->q[0], &v->q[1], sizeof(AVFrame *) * (size_t)(v->qn - 1));
                        v->q[v->qn - 1] = NULL;    /* vacated slot: no stale
                                                       pointer for close() */
                        v->qn--;
                        continue;
                }
                break;    /* head is in the future — clock not there yet */
        }

        /* frames were consumed: wake the decoder if it was waiting for
         * queue room (the missing signal froze playback at queue-depth
         * frames while the decode thread slept forever) */
        if (v->qn < before || v->qn < VQ_MAX)
                pthread_cond_broadcast(&v->cv);

        /* loop wrap (consumer side): the decoder hit EOF and is asleep on
         * wrap_pending. Wrap when the queue is drained AND the playhead has
         * reached the last frame's END — wrapping on a mere queue drain
         * truncates the final frame to a single tick (the last frame is
         * consumed at its START; its duration still has to play out).
         * cur_end == 0 covers "nothing ever decoded": wrap rather than
         * deadlock the decoder's wait. cur_pts resets with the playhead —
         * the no-timestamp fallback derives the next pts from it. */
        if (v->wrap_pending && v->qn == 0 && v->playhead >= v->cur_end) {
                v->playhead = 0;
                v->cur_pts = -1e18;
                v->wrap_pending = false;
                pthread_cond_broadcast(&v->cv);
        }
        const RVideoFrame *out = (v->cur && v->disp.w) ? &v->disp : NULL;
        pthread_mutex_unlock(&v->mu);
        return out;
}

u64 rev_video_dropped(RevVideo *v)
{
        return v ? v->dropped : 0;
}

/* CPU-tier conversion: either frame family -> straight-alpha RGBA8,
 * scaled to (dw x dh), written into a caller-placed rectangle whose row
 * length is `dst_stride` u32 elements (dw <= dst_stride). One sws pass,
 * no intermediate blit. Returns false when the frame is malformed. */
bool rev_video_to_rgba_at(const RVideoFrame *f, u32 *dst, u32 dst_stride,
                          u32 dw, u32 dh)
{
        if (!f || !dst || !dw || !dh || !g_ff_ok)
                return false;
        if (dst_stride < dw)
                return false;
        if (f->rgb) {
                if (!f->rgba || f->ls_rgba < (int)(f->w * 4))
                        return false;
                /* RGBA -> RGBA (scale + copy): alpha survives; the source
                 * is already in engine byte order (R,G,B,A in memory) */
                struct SwsContext *sws = g_ff.sws_getContext(
                        (int)f->w, (int)f->h, AV_PIX_FMT_RGBA,
                        (int)dw, (int)dh, AV_PIX_FMT_RGBA,
                        2 /* SWS_BILINEAR */, NULL, NULL, NULL);
                if (!sws)
                        return false;
                const uint8_t *planes[1] = { f->rgba };
                const int strides[1] = { f->ls_rgba };
                uint8_t *dstp = (uint8_t *)dst;
                int dst_ls = (int)dst_stride * 4;
                g_ff.sws_scale(sws, planes, strides, 0, (int)f->h,
                               &dstp, &dst_ls);
                g_ff.sws_freeContext(sws);
                return true;
        }
        /* YUV420P (BT.601) with any destination stride */
        if (!f->y || !f->u || !f->v)
                return false;
        struct SwsContext *sws = g_ff.sws_getContext((int)f->w, (int)f->h,
                                                     AV_PIX_FMT_YUV420P,
                                                     (int)dw, (int)dh,
                                                     AV_PIX_FMT_RGBA,
                                                     2 /* SWS_BILINEAR */,
                                                     NULL, NULL, NULL);
        if (!sws)
                return false;
        const uint8_t *planes[3] = { f->y, f->u, f->v };
        const int strides[3] = { f->ls_y, f->ls_u, f->ls_v };
        uint8_t *dstp = (uint8_t *)dst;
        int dst_ls = (int)dst_stride * 4;
        g_ff.sws_scale(sws, planes, strides, 0, (int)f->h, &dstp, &dst_ls);
        g_ff.sws_freeContext(sws);
        return true;
}

/* Tight-buffer convenience wrapper (scene layers, tests). The direct
 * at/sub conversions can spill up to one extra OUTPUT ROW past the last
 * (observed: 8 u32 past the final row on sub-rect calls through sws's
 * SIMD paths — an FFmpeg-internal behavior that varies by build and
 * width), so callers of those must pad. This wrapper owns a padded
 * internal buffer and copies out, keeping the historical dw*dh-exact
 * contract every existing caller was written against. Single-threaded
 * (render thread / --once only — never the decode thread). */
static u32 *g_to_rgba_tmp;
static size_t g_to_rgba_tmp_cap;      /* u32 elements */

bool rev_video_to_rgba(const RVideoFrame *f, u32 *dst, u32 dw, u32 dh)
{
        const size_t need = (size_t)dw * (dh + 1) + 16;
        if (g_to_rgba_tmp_cap < need) {
                u32 *np = (u32 *)realloc(g_to_rgba_tmp, need * 4);
                if (!np)
                        return false;
                g_to_rgba_tmp = np;
                g_to_rgba_tmp_cap = need;
        }
        if (!rev_video_to_rgba_at(f, g_to_rgba_tmp, dw, dw, dh))
                return false;
        memcpy(dst, g_to_rgba_tmp, (size_t)dw * dh * 4);
        return true;
}

bool rev_video_to_rgba_sub(const RVideoFrame *f, u32 sx, u32 sy,
                           u32 sw, u32 sh, u32 *dst, u32 dst_stride,
                           u32 dw, u32 dh)
{
        if (!f || !dst || !dw || !dh || !g_ff_ok)
                return false;
        if (!sw || !sh || dst_stride < dw)
                return false;
        if (f->rgb) {
                if (!f->rgba || f->ls_rgba < (int)(f->w * 4))
                        return false;
                if (sx + sw > f->w || sy + sh > f->h)
                        return false;
                const u8 *sub = f->rgba + (size_t)sy * (size_t)f->ls_rgba + (size_t)sx * 4;
                struct SwsContext *sws = g_ff.sws_getContext(
                        (int)sw, (int)sh, AV_PIX_FMT_RGBA,
                        (int)dw, (int)dh, AV_PIX_FMT_RGBA,
                        2 /* SWS_BILINEAR */, NULL, NULL, NULL);
                if (!sws)
                        return false;
                const uint8_t *planes[1] = { sub };
                const int strides[1] = { f->ls_rgba };
                uint8_t *dstp = (uint8_t *)dst;
                int dst_ls = (int)dst_stride * 4;
                g_ff.sws_scale(sws, planes, strides, 0, (int)sh, &dstp, &dst_ls);
                g_ff.sws_freeContext(sws);
                return true;
        }
        /* YUV420P: chroma is half resolution — offset both planes; sws
         * derives the chroma dimensions from the context's srcW/srcH */
        if (!f->y || !f->u || !f->v)
                return false;
        if (sx + sw > f->w || sy + sh > f->h)
                return false;
        const u32 cx = sx / 2, cy = sy / 2;
        struct SwsContext *sws = g_ff.sws_getContext((int)sw, (int)sh,
                                                     AV_PIX_FMT_YUV420P,
                                                     (int)dw, (int)dh,
                                                     AV_PIX_FMT_RGBA,
                                                     2 /* SWS_BILINEAR */,
                                                     NULL, NULL, NULL);
        if (!sws)
                return false;
        const uint8_t *planes[3] = {
                f->y + (size_t)sy * (size_t)f->ls_y + (size_t)sx,
                f->u + (size_t)cy * (size_t)f->ls_u + (size_t)cx,
                f->v + (size_t)cy * (size_t)f->ls_v + (size_t)cx
        };
        const int strides[3] = { f->ls_y, f->ls_u, f->ls_v };
        uint8_t *dstp = (uint8_t *)dst;
        int dst_ls = (int)dst_stride * 4;
        g_ff.sws_scale(sws, planes, strides, 0, (int)sh, &dstp, &dst_ls);
        g_ff.sws_freeContext(sws);
        return true;
}

f64 rev_video_fps(const RevVideo *v)
{
        return v ? v->fps : 0.0;
}
