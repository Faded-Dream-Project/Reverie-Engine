#!/bin/sh
# ============================================================================
# Reverie Engine — per-theme performance baseline / regression harness.
#
# Runs the daemon for a fixed window per (scene x renderer) under Xvfb at
# 1080p with REV_PROFILE=1 and prints the last perf lines. Software-GL
# (llvmpipe) numbers measure the CPU-side pipeline costs honestly — the
# fragment work itself is software, which is labeled in the output.
#
# Usage: scripts/bench.sh [seconds]     (default 12)
# ============================================================================
cd "$(dirname "$0")/.." || exit 1

SECS="${1:-12}"
XDISPLAY=":96"
OUT=$(mktemp -d)
XVFB_PID=""

GPU_ROOT="${REV_GPU_ROOT:-$(cd .. && pwd)/gpulibs/root}"
GPU_ENV=""
if [ -d "$GPU_ROOT/usr/lib/x86_64-linux-gnu" ]; then
    L="$GPU_ROOT/usr/lib/x86_64-linux-gnu"
    GPU_ENV="LD_LIBRARY_PATH=$L:$L/dri"
    if [ -f "$GPU_ROOT/usr/share/glvnd/egl_vendor.d/50_mesa.json" ]; then
        GPU_ENV="$GPU_ENV __EGL_VENDOR_LIBRARY_FILENAMES=$GPU_ROOT/usr/share/glvnd/egl_vendor.d/50_mesa.json"
    fi
fi

cleanup() {
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null && wait "$XVFB_PID" 2>/dev/null
}
trap cleanup EXIT INT TERM

Xvfb "$XDISPLAY" -screen 0 1920x1080x24 -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.5

SCENES="fluid-aurora ember-drift snowfall mountain-dusk ripple-lake"

for R in cpu opengl; do
    for S in $SCENES; do
        LOG="$OUT/${R}_${S}.log"
        # REV_FAKE_POINTER: deterministic drag so interactive layers (fluid
        # splats, ripple refraction, spawn=both particles) are exercised —
        # an idle cursor measures an idle scene.
        DISPLAY="$XDISPLAY" env $GPU_ENV REV_PROFILE=1 REV_FAKE_POINTER=0.15 \
            timeout "$SECS" \
            ./build/reverie -R "$R" "scenes/${S}.rev" > "$LOG" 2>&1 || true
        FPS=$(grep 'perf: .* fps' "$LOG" | tail -1)
        STAGES=$(grep 'perf: sim=' "$LOG" | tail -1)
        DIST=$(grep 'perf: frame' "$LOG" | tail -1)
        TIER=$(grep 'renderer: ' "$LOG" | tail -1)
        GPUFL=$(grep 'fluid simulation' "$LOG" | tail -1)
        echo "[$R] ${S}:"
        echo "      $TIER"
        [ -n "$GPUFL" ] && echo "      $GPUFL"
        echo "      $FPS"
        echo "      $STAGES"
        echo "      $DIST"
    done
done

echo "(software-GL box: 'render+present' on the opengl tier includes llvmpipe"
echo " fragment work; sim/raster are genuine CPU-side costs)"
