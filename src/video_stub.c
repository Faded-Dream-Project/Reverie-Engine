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
