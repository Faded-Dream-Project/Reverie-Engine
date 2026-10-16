#!/bin/sh
# ============================================================================
# Reverie Engine — Wayland E2E against a REAL compositor (weston headless).
#
# The raw wire client is exercised against a genuine libwayland-server
# implementation (weston + desktop-shell), not just our own fake compositor:
#
#   A. handshake:  get_registry + sync roundtrip must complete cleanly
#                  (no "protocol parse error", no "registry roundtrip timed
#                  out") and end at the expected "compositor lacks required
#                  globals" gate — weston 14 does not implement
#                  zwlr_layer_shell_v1, so the client must report that
#                  precisely, proving it parsed every global event.
#
#   B. golden bytes: our client's first 24 wire bytes are captured through a
#                  byte-logging relay and compared BYTE-EXACT against the
#                  bytes a real libwayland client (weston-simple-shm) sends:
#                     get_registry: 01 00 00 00 01 00 0c 00 02 00 00 00
#                     sync:         01 00 00 00 00 00 0c 00 03 00 00 00
#                  (captured from a live session; word 1 = (size<<16)|opcode)
#
#   C. wlshim GPU:  default-on shim through real libwayland against weston's
#                  registry flood + wl_output v4 name/description events —
#                  the exact field-crash conditions of the 0.1.4 segfault;
#                  C2 checks the REV_WAYLAND_GPU=0 opt-out.
#
# Requires the local test stack under $REV_GPU_ROOT (default:
# ../gpulibs relative to the repo — weston, libwayland, desktop-shell).
# Skips gracefully when the stack is absent.
#
# Usage: scripts/e2e-wayland.sh [scene.rev]   (default: themes/mountain-dusk/scene.rev)
# ============================================================================
cd "$(dirname "$0")/.." || exit 1

SCENE="${1:-themes/mountain-dusk/scene.rev}"
GPU_ROOT="${REV_GPU_ROOT:-$(cd .. && pwd)/gpulibs/root}"
WESTON="$GPU_ROOT/usr/bin/weston"
if [ ! -x "$WESTON" ]; then
    echo "e2e-wayland: SKIP (no weston test stack at $GPU_ROOT)"
    exit 0
fi

LIB="$GPU_ROOT/usr/lib/x86_64-linux-gnu"
OUT=$(mktemp -d)
XDG=/tmp/rev-e2e-wl.$$ 
mkdir -p "$XDG" && chmod 700 "$XDG"
SOCK="rev-e2e-0"
WESTON_PID=""
PASS=0; FAIL=0

# ---- build the byte relay (self-contained; cc is available wherever the
# ---- engine builds) ---------------------------------------------------------
cat > "$OUT/relay.c" <<'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
/* one-directional byte tap: LISTEN -> TARGET, C->S stream saved raw to FILE */
int main(int argc, char **argv)
{
    if (argc != 4) return 1;
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un la; memset(&la, 0, sizeof la);
    la.sun_family = AF_UNIX; strcpy(la.sun_path, argv[1]);
    unlink(argv[1]);
    if (bind(ls, (struct sockaddr *)&la, sizeof la) < 0) return 1;
    listen(ls, 1);
    int ca = accept(ls, NULL, NULL);
    if (ca < 0) return 1;
    int ct = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ta; memset(&ta, 0, sizeof ta);
    ta.sun_family = AF_UNIX; strcpy(ta.sun_path, argv[2]);
    if (connect(ct, (struct sockaddr *)&ta, sizeof ta) < 0) return 1;
    FILE *f = fopen(argv[3], "wb");
    unsigned char buf[16384];
    for (;;) {
        struct pollfd pf[2] = { { ca, POLLIN, 0 }, { ct, POLLIN, 0 } };
        int r = poll(pf, 2, 3000);
        if (r <= 0) break;
        if (pf[0].revents & POLLIN) {
            ssize_t n = read(ca, buf, sizeof buf);
            if (n <= 0) break;
            fwrite(buf, 1, (size_t)n, f); fflush(f);
            if (write(ct, buf, (size_t)n) != n) break;
        }
        if (pf[1].revents & POLLIN) {
            ssize_t n = read(ct, buf, sizeof buf);
            if (n <= 0) break;
            if (write(ca, buf, (size_t)n) != n) break;
        }
    }
    fclose(f);
    return 0;
}
EOF
cc -O2 -o "$OUT/relay" "$OUT/relay.c" 2>/dev/null || {
    echo "e2e-wayland: SKIP (cc unavailable to build the relay)"; rm -rf "$OUT" "$XDG"; exit 0; }

# LD_PRELOAD shim: weston's shell clients live under the extraction dir, not
# /usr/libexec (no root on the test box).
cat > "$OUT/libexec_shim.c" <<'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
char *wet_get_libexec_path(const char *name)
{
    char path[512];
    const char *root = getenv("REV_GPU_ROOT");
    snprintf(path, sizeof path, "%s/usr/libexec/%s",
             (root && *root) ? root : "/nonexistent", name);
    return strdup(path);
}
EOF
cc -shared -fPIC -o "$OUT/libexec_shim.so" "$OUT/libexec_shim.c" || exit 1

cleanup() {
    [ -n "$WESTON_PID" ] && kill "$WESTON_PID" 2>/dev/null
    sleep 0.2
    rm -rf "$OUT" "$XDG"
}
trap cleanup EXIT INT TERM

start_weston() {
    rm -f "$XDG/$SOCK" "$XDG/$SOCK.lock" "$XDG/relay"
    REV_GPU_ROOT="$GPU_ROOT" \
    LD_LIBRARY_PATH="$LIB:$LIB/libweston-14:$LIB/weston" \
    XDG_RUNTIME_DIR="$XDG" \
    XDG_DATA_HOME="$GPU_ROOT/usr/share" \
    LD_PRELOAD="$OUT/libexec_shim.so" \
    WESTON_MODULE_MAP="headless-backend.so=$LIB/libweston-14/headless-backend.so;desktop-shell.so=$LIB/weston/desktop-shell.so" \
    "$WESTON" --backend=headless-backend.so --socket="$SOCK" --idle-time=0 \
        > "$OUT/weston.log" 2>&1 &
    WESTON_PID=$!
    i=0
    while [ $i -lt 40 ]; do
        [ -S "$XDG/$SOCK" ] && break
        sleep 0.25
        i=$((i+1))
    done
    [ -S "$XDG/$SOCK" ] || { echo "e2e-wayland: weston did not start"; cat "$OUT/weston.log"; return 1; }
}

echo "reverie e2e-wayland — real weston, scene: $SCENE"

# ---- A. handshake against the real compositor --------------------------------
start_weston || exit 1
XDG_RUNTIME_DIR="$XDG" WAYLAND_DISPLAY="$SOCK" timeout 12 \
    ./build/reverie -B wayland "$SCENE" > "$OUT/a.log" 2>&1
if grep -q "protocol parse error" "$OUT/a.log" || grep -q "roundtrip timed out" "$OUT/a.log" \
   || grep -q "roundtrip failed" "$OUT/a.log"; then
    echo "    handshake:    FAIL (wire-level error against real weston)"
    FAIL=$((FAIL+1))
elif grep -q "zwlr_layer_shell_v1" "$OUT/a.log"; then
    echo "    handshake:    PASS (roundtrip clean; layer-shell absence reported exactly)"
    PASS=$((PASS+1))
else
    echo "    handshake:    FAIL (unexpected outcome)"
    FAIL=$((FAIL+1))
fi
kill "$WESTON_PID" 2>/dev/null; wait "$WESTON_PID" 2>/dev/null; WESTON_PID=""

# ---- B. golden bytes: our stream vs real libwayland's ------------------------
start_weston || exit 1
REV_GPU_ROOT="$GPU_ROOT" "$OUT/relay" "$XDG/relay" "$XDG/$SOCK" "$OUT/cli2srv.bin" &
RELAY_PID=$!
sleep 0.5
XDG_RUNTIME_DIR="$XDG" WAYLAND_DISPLAY=relay timeout 6 \
    ./build/reverie -B wayland "$SCENE" > "$OUT/b.log" 2>&1 || true
kill "$RELAY_PID" 2>/dev/null; wait "$RELAY_PID" 2>/dev/null
kill "$WESTON_PID" 2>/dev/null; wait "$WESTON_PID" 2>/dev/null; WESTON_PID=""

# golden: get_registry + sync, byte-captured from weston-simple-shm (libwayland 1.23)
printf '%b' '\001\000\000\000\001\000\014\000\002\000\000\000\001\000\000\000\000\000\014\000\003\000\000\000' > "$OUT/golden.bin"
if [ ! -s "$OUT/cli2srv.bin" ]; then
    echo "    golden bytes: FAIL (client stream not captured)"
    FAIL=$((FAIL+1))
elif cmp -s "$OUT/golden.bin" -n 24 "$OUT/cli2srv.bin" 2>/dev/null; then
    echo "    golden bytes: PASS (first 24 bytes == real libwayland's)"
    PASS=$((PASS+1))
else
    echo "    golden bytes: FAIL (client bytes diverge from libwayland's)"
    od -An -tx1 -N32 "$OUT/cli2srv.bin" 2>/dev/null | head -2
    FAIL=$((FAIL+1))
fi

# ---- C. GPU shim against the real compositor (default ON since the
#      "use the GPU that is present" rework) ---------------------------------
# No env var: the wlshim now engages by default, runs weston's full registry
# flood + the post-bind event burst (wl_output v4 name/description events,
# opcodes 4/5 — the 0.1.4 segfault conditions), then falls back to the raw
# client at the clean layer-shell gate (weston does not implement it).
start_weston || exit 1
XDG_RUNTIME_DIR="$XDG" WAYLAND_DISPLAY="$SOCK" timeout 12 \
    ./build/reverie -B wayland "$SCENE" > "$OUT/c.log" 2>&1
if grep -q "listener function for opcode" "$OUT/c.log"; then
    echo "    wlshim GPU:    FAIL (listener table too short — real compositor events)"
    FAIL=$((FAIL+1))
elif grep -q "wlshim: compositor lacks wl_compositor or zwlr_layer_shell_v1" "$OUT/c.log" \
     && grep -q "wayland GPU WSI unavailable" "$OUT/c.log"; then
    echo "    wlshim GPU:    PASS (registry flood dispatched; clean fallback to raw client)"
    PASS=$((PASS+1))
else
    echo "    wlshim GPU:    FAIL (unexpected outcome)"
    cat "$OUT/c.log"
    FAIL=$((FAIL+1))
fi
kill "$WESTON_PID" 2>/dev/null; wait "$WESTON_PID" 2>/dev/null; WESTON_PID=""

# ---- C2. explicit opt-out: REV_WAYLAND_GPU=0 keeps the raw client ------------
start_weston || exit 1
XDG_RUNTIME_DIR="$XDG" WAYLAND_DISPLAY="$SOCK" timeout 12 \
    env REV_WAYLAND_GPU=0 ./build/reverie -B wayland "$SCENE" > "$OUT/c2.log" 2>&1
if grep -q "wlshim" "$OUT/c2.log"; then
    echo "    wlshim optout: FAIL (shim engaged despite REV_WAYLAND_GPU=0)"
    FAIL=$((FAIL+1))
elif grep -q "zwlr_layer_shell_v1" "$OUT/c2.log"; then
    echo "    wlshim optout: PASS (raw client only, own layer-shell gate)"
    PASS=$((PASS+1))
else
    echo "    wlshim optout: FAIL (unexpected outcome)"
    cat "$OUT/c2.log"
    FAIL=$((FAIL+1))
fi
kill "$WESTON_PID" 2>/dev/null; wait "$WESTON_PID" 2>/dev/null; WESTON_PID=""

echo "==================================================="
echo "e2e-wayland: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
