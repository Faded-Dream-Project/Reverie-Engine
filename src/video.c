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
        AVFrame *cur;         /* frame being displayed (ref'd)            */
        f64 cur_pts;
        bool dead;            /* irrecoverable error / shutdown           */
        bool ended;           /* stream ended (non-loop)                  */
        u64 dropped;

        /* display side (render thread) */
        f64 playhead;
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

static void *decode_thread(void *arg)
{
        RevVideo *v = (RevVideo *)arg;

        for (;;) {
                pthread_mutex_lock(&v->mu);
                while (!v->dead && v->qn == VQ_MAX)
                        pthread_cond_wait(&v->cv, &v->mu);
                bool dead = v->dead;
                pthread_mutex_unlock(&v->mu);
                if (dead)
                        break;

                int rc = g_ff.av_read_frame(v->fmt, v->pkt);
                if (rc < 0) {
                        /* EOF or read error: loop or finish */
                        if (v->loop &&
                            g_ff.av_seek_frame(v->fmt, -1, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
                                g_ff.avcodec_flush_buffers(v->cdc);
                                g_ff.av_packet_unref(v->pkt);
                                pthread_mutex_lock(&v->mu);
                                v->playhead = 0;
                                v->ended = false;
                                for (int i = 0; i < v->qn; i++) {
                                        g_ff.av_frame_free(&v->q[i]);
                                        v->q[i] = NULL;
                                }
                                v->qn = 0;
                                pthread_cond_broadcast(&v->cv);
                                pthread_mutex_unlock(&v->mu);
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
                        pthread_mutex_lock(&v->mu);
                        if (v->dead) {
                                g_ff.av_frame_free(&f);
                                pthread_mutex_unlock(&v->mu);
                                break;
                        }
                        if (v->qn == VQ_MAX) {
                                /* consumer fell behind: drop the oldest */
                                g_ff.av_frame_free(&v->q[0]);
                                memmove(&v->q[0], &v->q[1],
                                        sizeof(AVFrame *) * (VQ_MAX - 1));
                                v->q[VQ_MAX - 1] = f;
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
                snprintf(err, errcap,
                         "video: FFmpeg libraries not usable on this system "
                         "(see the log for the reason)");
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
        LOGI("video: %dx%d (%s), loop=%s", v->cdc->width, v->cdc->height,
             dec->name, loop ? "yes" : "no");

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
                                v->disp.y = v->cur->data[0];
                                v->disp.u = v->cur->data[1];
                                v->disp.v = v->cur->data[2];
                                v->disp.ls_y = v->cur->linesize[0];
                                v->disp.ls_u = v->cur->linesize[1];
                                v->disp.ls_v = v->cur->linesize[2];
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
        const RVideoFrame *out = (v->cur && v->disp.w) ? &v->disp : NULL;
        pthread_mutex_unlock(&v->mu);
        return out;
}

u64 rev_video_dropped(RevVideo *v)
{
        return v ? v->dropped : 0;
}

/* CPU-tier fallback: YUV420P -> straight-alpha RGBA8 at (dw x dh) through
 * swscale. Returns false when the frame format is not what we expect (the
 * caller just skips the layer for that frame). */
bool rev_video_to_rgba(const RVideoFrame *f, u32 *dst, u32 dw, u32 dh)
{
        if (!f || !dst || !dw || !dh || !g_ff_ok)
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
        int dst_stride = (int)dw * 4;
        g_ff.sws_scale(sws, planes, strides, 0, (int)f->h, &dstp, &dst_stride);
        g_ff.sws_freeContext(sws);
        return true;
}
