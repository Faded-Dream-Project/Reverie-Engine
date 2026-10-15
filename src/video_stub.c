/* ============================================================================
 * Reverie Engine — video_stub.c
 * Built INSTEAD of video.c when no FFmpeg headers are available: the
 * engine keeps its zero-dependency build, and video layers explain
 * themselves instead of silently vanishing.
 *
 * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.
 * ==========================================================================*/
#include "video.h"

#include <stdio.h>

#include "log.h"

bool rev_video_available(void)
{
        static bool said;
        if (!said) {
                said = true;
                LOGI("video: built without FFmpeg headers — video wallpapers "
                     "unavailable (install libavcodec-dev etc. and rebuild; "
                     "see docs/VIDEO.md)");
        }
        return false;
}

RevVideo *rev_video_open(const RVideoSource *src, bool loop,
                         char *err, size_t errcap)
{
        (void)src;
        (void)loop;
        snprintf(err, errcap,
                 "video: this reverie build has no video support "
                 "(built without FFmpeg headers)");
        return NULL;
}

void rev_video_close(RevVideo *v)
{
        (void)v;
}

const RVideoFrame *rev_video_display(RevVideo *v, f64 dt)
{
        (void)v;
        (void)dt;
        return NULL;
}

u64 rev_video_dropped(RevVideo *v)
{
        (void)v;
        return 0;
}

bool rev_video_to_rgba(const RVideoFrame *f, u32 *dst, u32 dw, u32 dh)
{
        (void)f; (void)dst; (void)dw; (void)dh;
        return false;
}

bool rev_video_to_rgba_at(const RVideoFrame *f, u32 *dst, u32 dst_stride,
                          u32 dw, u32 dh)
{
        (void)f; (void)dst; (void)dst_stride; (void)dw; (void)dh;
        return false;
}

bool rev_video_to_rgba_sub(const RVideoFrame *f, u32 sx, u32 sy,
                           u32 sw, u32 sh, u32 *dst, u32 dst_stride,
                           u32 dw, u32 dh)
{
        (void)f; (void)sx; (void)sy; (void)sw; (void)sh;
        (void)dst; (void)dst_stride; (void)dw; (void)dh;
        return false;
}

f64 rev_video_fps(const RevVideo *v)
{
        (void)v;
        return 0.0;
}
