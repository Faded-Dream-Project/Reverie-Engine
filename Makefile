# ============================================================================
#  Reverie Engine — build file.
#
#  Requires nothing but a C11 compiler, make, and the C standard library.
#  No Xlib. No zlib. No libpng. No pthreads. No external anything.
#
#    make            build reverie + revtool
#    make test       build & run the test suite
#    make assets     regenerate bundled scene assets (uses revtool itself)
#    make install    install binaries to PREFIX (default /usr/local)
#    make clean      remove build artifacts
# ============================================================================

CC      ?= cc
AR      ?= ar
PREFIX  ?= /usr/local

# -O2 default; override on the command line if desired (e.g. CFLAGS=-O0 -g)
CFLAGS  ?= -O2

WARN     = -Wall -Wextra -Werror -Wvla -Wshadow -Wpointer-arith -Wpedantic
DEFS     = -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
OPTS     = -fno-math-errno -funroll-loops
ALL_CFLAGS  = -std=c11 $(WARN) $(DEFS) $(CFLAGS) $(OPTS) -Isrc
LDFLAGS     =
LIBS        = -lm -ldl -lpthread

BUILD   = build

# ---- optional video support (FFmpeg headers at build, dlopen at run) --------
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

# ---- core engine objects (everything except the X11 backend) ---------------
CORE_SRC = \
	src/arena.c \
	src/autostart.c \
	src/cli.c \
	src/log.c \
	src/util.c \
	src/simd.c \
	src/inflate.c \
	src/png.c \
	src/pack.c \
	src/geom.c \
	src/raster.c \
	src/svg.c \
	src/rev.c \
	src/json.c \
	src/theme.c \
	src/scene.c \
	src/composite.c \
	src/fluid.c \
	src/ripple.c \
	src/particles.c \
	src/platform.c \
	src/ipc.c \
	src/x11.c \
	src/plat_x11.c \
	src/plat_wayland.c \
	src/wayland.c \
	src/wlshim.c \
	src/render.c \
	src/render_cpu.c \
	src/render_gl.c \
	src/render_vk.c

CORE_OBJ = $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRC)) $(VIDEO_OBJ)
LIBREV   = $(BUILD)/libreverie.a

# video.o needs the FFmpeg headers (system or vendored); video_stub.o needs
# nothing - whichever was selected by the AV_INC detection above
$(BUILD)/video.o: src/video.c src/video.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) $(VIDEO_CPPF) -c $< -o $@
$(BUILD)/video_stub.o: src/video_stub.c src/video.h | $(BUILD)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

# ---- executables -------------------------------------------------------------
.DEFAULT_GOAL := all
BINS  = $(BUILD)/reverie $(BUILD)/revtool

# ---- tests -------------------------------------------------------------------
TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

.PHONY: all test assets install clean

all: $(BINS)

$(BUILD):
	@mkdir -p $(BUILD)

# Header deps: every object depends on ALL src/*.h — including the
# GENERATED src/vk_spv.h. Without this, regenerating the embedded SPIR-V
# (scripts/gen_spv.sh) leaves render_vk.o stale and the new kernels
# silently never reach the binary (bit this project twice: platform.h in
# the 0.2.0 staging rework, vk_spv.h in the byte-swap fix — caught only
# by scripts/verify-bgra-flag.sh). Over-broad recompilation is cheap;
# stale objects are not.
$(BUILD)/%.o: src/%.c $(wildcard src/*.h) | $(BUILD)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

# delete-then-create: 'ar r' MERGES members, so switching ENABLE_FFMPEG
# would leave a stale video.o inside the archive next to video_stub.o
$(LIBREV): $(CORE_OBJ)
	rm -f $@
	$(AR) rcs $@ $(CORE_OBJ)

$(BUILD)/reverie: $(BUILD)/main.o $(LIBREV)
	$(CC) $(ALL_CFLAGS) -o $@ $(BUILD)/main.o $(LIBREV) $(LDFLAGS) $(LIBS)

$(BUILD)/revtool: $(BUILD)/revtool.o $(LIBREV)
	$(CC) $(ALL_CFLAGS) -o $@ $(BUILD)/revtool.o $(LIBREV) $(LDFLAGS) $(LIBS)

$(BUILD)/%.o: tests/%.c $(wildcard src/*.h) | $(BUILD)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(BUILD)/test_%: $(BUILD)/test_%.o $(LIBREV)
	$(CC) $(ALL_CFLAGS) -o $@ $< $(LIBREV) $(LDFLAGS) $(LIBS) -lpthread

test: $(TEST_BIN)
	@fail=0; \
	for t in $(TEST_BIN); do \
	        printf '  RUN  %s\n' "$$t"; \
	        "$$t" || { printf '  FAIL %s\n' "$$t"; fail=1; }; \
	done; \
	[ $$fail -eq 0 ] && echo '  ALL TESTS PASSED' || exit 1

# Scene assets are produced by Reverie's own PNG encoder (dogfooding).
assets: $(BUILD)/revtool
	$(BUILD)/revtool mkpng themes/ripple-lake/assets/lake.png 512 512 soft-bands
	$(BUILD)/revtool mkpng themes/mountain-dusk/assets/stars.png 512 512 stars
	$(BUILD)/revtool mkpng themes/nebula-drift/assets/stars.png 1920 1080 stars

install: $(BINS)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/reverie $(DESTDIR)$(PREFIX)/bin/reverie
	install -m 755 $(BUILD)/revtool $(DESTDIR)$(PREFIX)/bin/revtool

clean:
	rm -rf $(BUILD)
