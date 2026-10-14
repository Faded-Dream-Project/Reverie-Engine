# Vendored FFmpeg headers (Debian 13 / FFmpeg 7.1, LGPLv2.1+)

These are the installed `libavcodec/libavformat/libavutil/libswscale`
development headers from Debian 13's `ffmpeg 7.1.5-0+deb13u1`, extracted
with `dpkg-deb -x` from the distro's dev packages (no root needed).

Why they are here: `src/video.c` compiles against real FFmpeg struct
layouts while still loading the LIBRARIES with `dlopen` at runtime —
zero link-time dependencies, the engine runs without FFmpeg installed.
The Makefile prefers system headers when present (matching the user's
distro exactly) and falls back to these. A runtime ABI-major check
refuses video cleanly when the loaded libraries do not match the
headers this build was compiled against.

Licensing: these headers are part of FFmpeg (LGPLv2.1+). Redistributing
them is standard practice; the engine does not link FFmpeg — it loads
the user's system libraries at runtime, which keeps the LGPL components
dynamically replaceable. See docs/VIDEO.md for the full architecture
and licensing analysis.
