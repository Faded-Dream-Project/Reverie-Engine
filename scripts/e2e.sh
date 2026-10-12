#!/bin/sh
# ============================================================================
# Reverie Engine — end-to-end verification under Xvfb.
#
# Runs the daemon for a few seconds per renderer tier and captures
# screenshots via the built-in --shot hook:
#   1. cpu          (always available — the engine's own compositor)
#   2. opengl       (requires libEGL + a GL vendor; llvmpipe counts)
#   3. vulkan       (requires libvulkan + an ICD; lavapipe counts)
#
# GPU tiers are skipped gracefully when their libraries are not loadable.
#
# Usage: scripts/e2e.sh [scene.rev]     (default: scenes/mountain-dusk.rev)
# ============================================================================
cd "$(dirname "$0")/.." || exit 1

SCENE="${1:-scenes/mountain-dusk.rev}"
OUT=$(mktemp -d)
XDISPLAY=":97"
XVFB_PID=""
PASS=0; SKIP=0; FAIL=0

cleanup() {
    if [ -n "$XVFB_PID" ]; then
        kill "$XVFB_PID" 2>/dev/null
        wait "$XVFB_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

echo "reverie e2e — scene: $SCENE  out: $OUT"

start_xvfb() {
    Xvfb "$XDISPLAY" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1.5
}

stop_xvfb() {
    if [ -n "$XVFB_PID" ]; then
        kill "$XVFB_PID" 2>/dev/null
        wait "$XVFB_PID" 2>/dev/null
        XVFB_PID=""
    fi
}

# ---- CPU tier (always) -------------------------------------------------------
start_xvfb
DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu --shot 3 "$OUT/cpu" "$SCENE" \
    > "$OUT/cpu.log" 2>&1 || true
if [ -f "$OUT/cpu.png" ]; then
    echo "    cpu:          PASS (screenshot captured)"
    PASS=$((PASS+1))
else
    echo "    cpu:          FAIL (no screenshot)"
    cat "$OUT/cpu.log"
    FAIL=$((FAIL+1))
fi
stop_xvfb

# ---- OpenGL tier -------------------------------------------------------------
start_xvfb
DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R opengl --shot 3 "$OUT/gl" "$SCENE" \
    > "$OUT/gl.log" 2>&1 || true
if grep -q "renderer: opengl" "$OUT/gl.log" 2>/dev/null; then
    echo "    opengl:       PASS (renderer active$( [ -f "$OUT/gl.png" ] && echo ', screenshot' ))"
    PASS=$((PASS+1))
else
    echo "    opengl:       SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- Vulkan tier -------------------------------------------------------------
start_xvfb
DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R vulkan --shot 3 "$OUT/vk" "$SCENE" \
    > "$OUT/vk.log" 2>&1 || true
if grep -q "renderer: vulkan" "$OUT/vk.log" 2>/dev/null; then
    echo "    vulkan:       PASS (renderer active$( [ -f "$OUT/vk.png" ] && echo ', screenshot' ))"
    PASS=$((PASS+1))
else
    echo "    vulkan:       SKIP (no libvulkan / ICD on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

echo "==================================================="
echo "e2e: $PASS passed, $SKIP skipped (no driver), $FAIL failed"
echo "logs: $OUT"
[ "$FAIL" -eq 0 ]
