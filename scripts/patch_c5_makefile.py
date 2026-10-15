#!/usr/bin/env python3
"""C5 patcher: Makefile ENABLE_FFMPEG knob.

- ENABLE_FFMPEG=auto (default): today's behavior — system headers, else
  the vendored contrib set, else the stub.
- ENABLE_FFMPEG=1: video required; no headers anywhere is a hard
  $(error) naming the packages.
- ENABLE_FFMPEG=0: always the stub — no headers, no video symbol.

Also: the header search gains the compiler's multiarch directory
(cc -print-multiarch) ahead of the hardcoded x86_64 Debian path, and
the libreverie.a rule deletes the archive before ar rcs — 'ar r'
MERGES members, so switching ENABLE_FFMPEG would otherwise leave a
stale video.o inside the library.

Byte-exact python splice (recipes are TAB-indented; the edit tool
would destroy them).
"""

PATH = "Makefile"

with open(PATH, "rb") as f:
    mk = f.read().decode("utf-8")

OLD = """# ---- optional video support (FFmpeg headers at build, dlopen at run) --------
# System headers first (matches the user's distro exactly); the vendored
# contrib/ffmpeg-headers (Debian 13 / FFmpeg 7.1) as fallback. Without either,
# video_stub.c builds and the engine is exactly as before.
AV_DIRS := /usr/include/x86_64-linux-gnu /usr/include
AV_INC  := $(firstword $(foreach d,$(AV_DIRS),$(wildcard $(d)/libavcodec/avcodec.h)))
ifeq ($(AV_INC),)
AV_INC  := $(firstword $(wildcard contrib/ffmpeg-headers/libavcodec/avcodec.h))
endif
ifneq ($(AV_INC),)
AV_DIR  := $(patsubst %/libavcodec/avcodec.h,%,$(AV_INC))
VIDEO_OBJ  = $(BUILD)/video.o
VIDEO_CPPF = -DREV_VIDEO=1 -I$(AV_DIR)
else
VIDEO_OBJ  = $(BUILD)/video_stub.o
VIDEO_CPPF =
endif
"""

NEW = """# ---- optional video support (FFmpeg headers at build, dlopen at run) --------
# ENABLE_FFMPEG:
#   auto (default) - system headers when present, else the vendored
#                    contrib/ffmpeg-headers (Debian 13 / FFmpeg 7.1),
#                    else video_stub.c (no video, everything else identical)
#   1              - video REQUIRED: same header search, but finding none
#                    is a hard error naming the packages (install them or
#                    build with ENABLE_FFMPEG=0)
#   0              - always the stub: no FFmpeg headers, no video symbol;
#                    .rev scenes and every other feature build unchanged
# The LIBRARIES are still dlopen'd at runtime with an ABI guard in every
# configuration - nothing is ever linked.
ENABLE_FFMPEG ?= auto

# Header search order: the compiler's multiarch include directory first
# (matches the running distro exactly - Debian/Ubuntu multiarch, Fedora/
# Arch/Alpine/openSUSE plain /usr/include), then the vendored set.
MULTIARCH := $(shell $(CC) -print-multiarch 2>/dev/null)
AV_DIRS := /usr/include/$(MULTIARCH) /usr/include/x86_64-linux-gnu /usr/include
AV_INC  := $(firstword $(foreach d,$(AV_DIRS),$(wildcard $(d)/libavcodec/avcodec.h)))
ifeq ($(AV_INC),)
AV_INC  := $(firstword $(wildcard contrib/ffmpeg-headers/libavcodec/avcodec.h))
endif

ifeq ($(ENABLE_FFMPEG),0)
VIDEO_OBJ  = $(BUILD)/video_stub.o
VIDEO_CPPF =
else
ifeq ($(AV_INC),)
ifeq ($(ENABLE_FFMPEG),1)
$(error ENABLE_FFMPEG=1 requires FFmpeg headers - install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev (distro names vary) or build with ENABLE_FFMPEG=0)
endif
VIDEO_OBJ  = $(BUILD)/video_stub.o
VIDEO_CPPF =
else
AV_DIR  := $(patsubst %/libavcodec/avcodec.h,%,$(AV_INC))
VIDEO_OBJ  = $(BUILD)/video.o
VIDEO_CPPF = -DREV_VIDEO=1 -I$(AV_DIR)
endif
endif
"""

assert mk.count(OLD) == 1, "AV block anchor not found"
mk = mk.replace(OLD, NEW, 1)

# the archive rule: delete before ar rcs ('ar r' merges members - a stale
# video.o would survive an ENABLE_FFMPEG switch and could shadow the stub)
OLD_LIB = """$(LIBREV): $(CORE_OBJ)
\t$(AR) rcs $@ $(CORE_OBJ)
"""
NEW_LIB = """# delete-then-create: 'ar r' MERGES members, so switching ENABLE_FFMPEG
# would leave a stale video.o inside the archive next to video_stub.o
$(LIBREV): $(CORE_OBJ)
\trm -f $@
\t$(AR) rcs $@ $(CORE_OBJ)
"""
assert mk.count(OLD_LIB) == 1, "LIBREV rule anchor not found"
mk = mk.replace(OLD_LIB, NEW_LIB, 1)

with open(PATH, "wb") as f:
    f.write(mk.encode("utf-8"))
print("patched: ENABLE_FFMPEG knob + multiarch probe + archive rebuild")
print("tab recipe lines:", mk.count("\n\t"))
