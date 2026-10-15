# Reverie — Building on Linux distributions

The engine is written against **C11 + POSIX.1-2008** and dlopens every
heavy dependency (EGL, Vulkan, libwayland-client, the FFmpeg libraries)
at runtime. Build requirements: a C11 compiler, `make`, and — for video
support — FFmpeg **development headers** (never linked, always dlopen'd
with an ABI guard). Nothing here needs systemd, Bash, glibc, or GNU-only
utilities; the test scripts are POSIX `sh`.

FFmpeg at build time:

```
make                      # auto: system headers, else the vendored
                          # contrib/ffmpeg-headers (FFmpeg 7.1), else the
                          # no-video stub
make ENABLE_FFMPEG=0      # stub: no headers needed, no video
make ENABLE_FFMPEG=1      # required: a headerless build is a hard error
```

## Tier 1 — package names

| Distribution | Build + unit test install |
|---|---|
| **Debian / Ubuntu** | `sudo apt install build-essential libavcodec-dev libavformat-dev libavutil-dev libswscale-dev` (the `ffmpeg` CLI additionally provides the fixtures the video e2e generates) |
| **Arch** | `sudo pacman -S base-devel ffmpeg` — the `ffmpeg` package carries the development headers |
| **Fedora** | `sudo dnf install gcc make ffmpeg-devel` |
| **openSUSE Tumbleweed** | `sudo zypper install gcc make 'pkgconfig(libavcodec)' 'pkgconfig(libavformat)' 'pkgconfig(libavutil)' 'pkgconfig(libswscale)'` — the `pkgconfig(...)` form resolves the current versioned package names (`ffmpeg-7-libavcodec-devel`, ...) automatically |
| **openSUSE Leap** | Same command as Tumbleweed when your Leap release ships FFmpeg in the official repositories; otherwise enable Packman first (`zypper ar -f https://ftp.gwdg.de/pub/linux/misc/packman/suse/openSUSE_Leap_<version>/ packman && zypper ref`) — full FFmpeg lives there for patent reasons |

Debian-family multiarch (amd64/i386/arm64 on one system): the Makefile
probes `cc -print-multiarch` and then the plain include root, so headers
are found on every architecture without editing anything.

## Tier 2

| Distribution | Notes |
|---|---|
| **Void Linux (glibc)** | `sudo xbps-install base-devel ffmpeg-devel` — one toolchain package, headers included |
| **Void Linux (musl)** | same names from the musl repository; the engine's musl compatibility is the same code path as Alpine's (below) |
| **Alpine (musl + BusyBox)** | `sudo apk add build-base ffmpeg-dev ffmpeg` — `build-base` is gcc + musl + make; no glibc-isms are used (`SO_PEERCRED` is guarded, `system()` consumers check results, scripts are BusyBox-`sh` compatible) |

## Runtime requirements (all distributions)

* `.rev` scenes: none beyond libc/libm/libpthread/libdl — the CPU
  renderer and both display backends (raw X11, raw Wayland) are pure C.
* GPU tiers: the vendor driver's `libEGL.so.1` / `libvulkan.so.1` —
  loaded at runtime; absence degrades to the CPU renderer (with
  `--renderer auto`) or is a clear fatal error (explicit request).
* Media mode: the system's FFmpeg libraries (`libavcodec`,
  `libavformat`, `libavutil`, `libswscale`), any of the sonames
  FFmpeg 58-61 era, dlopen'd at runtime with a major-version ABI guard.
  A stub build (`ENABLE_FFMPEG=0`) refuses `--video` with a clear
  message and does not need these at all.
* Autostart: an XDG-Autostart-aware session; systemd is optional
  (docs/AUTOSTART.md).

## What is actually verified where

* **CI containers** (`.github/workflows/ci.yml`, job `distro-matrix`):
  Alpine (musl), Arch, Debian, Fedora, openSUSE Tumbleweed, and Void
  each build with their own toolchain and FFmpeg, run the full unit
  suite, then rebuild with `ENABLE_FFMPEG=0` and run it again. That is
  the automated portability evidence — real compilers, real library
  majors, real linkers.
* **CI (Ubuntu 24.04)**: the full chain — build, unit, Xvfb e2e across
  all renderer tiers, the FFmpeg matrix (system / vendored / off), and
  the sanitizer pass.
* **Manual / field**: Wayland layer-shell behavior on your compositor,
  hardware GPU timing, DPMS on real displays, login-session autostart.
  A distribution not listed here is not claimed to be tested; the code
  avoids the usual portability traps, but "should work" is not evidence.

## Known distribution behaviors

* Debian/Ubuntu patch glibc so `system()` is `__warn_unused_result`;
  all call sites consume the result (a CI failure taught us this).
* Debian's FFmpeg 6.1 vs the vendored 7.1 headers: the ABI guard
  refuses video cleanly instead of guessing — build against your
  distro's headers (`make` prefers them) or accept the refusal.
* Alpine's BusyBox `sh` runs the test scripts fine; `dash` (Debian)
  likewise — the scripts avoid Bashisms deliberately.
