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
LIBS        = -lm -ldl

BUILD   = build

# ---- core engine objects (everything except the X11 backend) ---------------
CORE_SRC = \
	src/arena.c \
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
	src/scene.c \
	src/composite.c \
	src/fluid.c \
	src/ripple.c \
	src/particles.c \
	src/platform.c \
	src/x11.c \
	src/plat_x11.c \
	src/plat_wayland.c \
	src/wayland.c \
	src/wlshim.c \
	src/render.c \
	src/render_cpu.c \
	src/render_gl.c \
	src/render_vk.c

CORE_OBJ = $(patsubst src/%.c,$(BUILD)/%.o,$(CORE_SRC))
LIBREV   = $(BUILD)/libreverie.a

# ---- executables -------------------------------------------------------------
BINS  = $(BUILD)/reverie $(BUILD)/revtool

# ---- tests -------------------------------------------------------------------
TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

.PHONY: all test assets install clean

all: $(BINS)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(LIBREV): $(CORE_OBJ)
	$(AR) rcs $@ $(CORE_OBJ)

$(BUILD)/reverie: $(BUILD)/main.o $(LIBREV)
	$(CC) $(ALL_CFLAGS) -o $@ $(BUILD)/main.o $(LIBREV) $(LDFLAGS) $(LIBS)

$(BUILD)/revtool: $(BUILD)/revtool.o $(LIBREV)
	$(CC) $(ALL_CFLAGS) -o $@ $(BUILD)/revtool.o $(LIBREV) $(LDFLAGS) $(LIBS)

$(BUILD)/%.o: tests/%.c | $(BUILD)
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
	$(BUILD)/revtool mkpng scenes/assets/lake.png 512 512 soft-bands
	$(BUILD)/revtool mkpng scenes/assets/stars.png 512 512 stars

install: $(BINS)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/reverie $(DESTDIR)$(PREFIX)/bin/reverie
	install -m 755 $(BUILD)/revtool $(DESTDIR)$(PREFIX)/bin/revtool

clean:
	rm -rf $(BUILD)
