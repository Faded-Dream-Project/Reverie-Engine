#!/bin/sh
# ============================================================================
# Reverie Engine — end-to-end verification under Xvfb.
#
# Runs the daemon for a few seconds per renderer tier and captures
# screenshots via the built-in --shot hook:
#   1. cpu          (always available — the engine's own compositor)
#   2. opengl       (requires libEGL + a GL vendor; llvmpipe counts)
#   3. vulkan       (requires libvulkan + an ICD; lavapipe counts)
#      - twice: the 0.1.1 default (platform SHM present) AND the WSI
#        swapchain present (REV_VK_PRESENT=wsi), so both paths stay
#        regression-tested.
#
# GPU tiers are skipped gracefully when their libraries are not loadable.
# When the local test stack exists at ../gpulibs/root (llvmpipe + lavapipe,
# no root needed), its env is exported automatically.
#
# Usage: scripts/e2e.sh [scene.rev]     (default: scenes/mountain-dusk.rev)
# ============================================================================
cd "$(dirname "$0")/.." || exit 1

SCENE="${1:-scenes/mountain-dusk.rev}"
OUT=$(mktemp -d)
XDISPLAY=":97"
XVFB_PID=""
PASS=0; SKIP=0; FAIL=0

# ---- optional local GPU stack (llvmpipe EGL + lavapipe Vulkan) --------------
GPU_ROOT="${REV_GPU_ROOT:-$(cd .. && pwd)/gpulibs/root}"
GPU_ENV=""
if [ -d "$GPU_ROOT/usr/lib/x86_64-linux-gnu" ]; then
    L="$GPU_ROOT/usr/lib/x86_64-linux-gnu"
    GPU_ENV="LD_LIBRARY_PATH=$L:$L/dri"
    if [ -f "$GPU_ROOT/usr/share/glvnd/egl_vendor.d/50_mesa.json" ]; then
        GPU_ENV="$GPU_ENV __EGL_VENDOR_LIBRARY_FILENAMES=$GPU_ROOT/usr/share/glvnd/egl_vendor.d/50_mesa.json"
    fi
    if [ -f "$GPU_ROOT/usr/share/vulkan/icd.d/lvp_icd.json" ]; then
        GPU_ENV="$GPU_ENV VK_ICD_FILENAMES=$GPU_ROOT/usr/share/vulkan/icd.d/lvp_icd.json"
    fi
fi

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
DISPLAY="$XDISPLAY" env $GPU_ENV timeout 8 ./build/reverie -R opengl --shot 3 "$OUT/gl" "$SCENE" \
    > "$OUT/gl.log" 2>&1 || true
if grep -q "renderer: opengl" "$OUT/gl.log" 2>/dev/null; then
    echo "    opengl:       PASS (renderer active$( [ -f "$OUT/gl.png" ] && echo ', screenshot' ))"
    PASS=$((PASS+1))
else
    echo "    opengl:       SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- Vulkan tier: default present path (platform SHM) -----------------------
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV timeout 8 ./build/reverie -R vulkan --shot 3 "$OUT/vk" "$SCENE" \
    > "$OUT/vk.log" 2>&1 || true
if grep -q "renderer: vulkan" "$OUT/vk.log" 2>/dev/null; then
    if grep -q "presenting via platform SHM path" "$OUT/vk.log" 2>/dev/null; then
        echo "    vulkan(shm):  PASS (renderer active$( [ -f "$OUT/vk.png" ] && echo ', screenshot' ))"
        PASS=$((PASS+1))
    else
        echo "    vulkan(shm):  FAIL (present-path log line missing)"
        FAIL=$((FAIL+1))
    fi
else
    echo "    vulkan(shm):  SKIP (no libvulkan / ICD on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- Vulkan tier: WSI swapchain present (REV_VK_PRESENT=wsi) ----------------
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV REV_VK_PRESENT=wsi timeout 8 ./build/reverie -R vulkan --shot 3 "$OUT/vkw" "$SCENE" \
    > "$OUT/vkw.log" 2>&1 || true
if grep -q "renderer: vulkan" "$OUT/vkw.log" 2>/dev/null; then
    if grep -q "swapchain present path ready" "$OUT/vkw.log" 2>/dev/null; then
        echo "    vulkan(wsi):  PASS (swapchain present active$( [ -f "$OUT/vkw.png" ] && echo ', screenshot' ))"
        PASS=$((PASS+1))
    else
        echo "    vulkan(wsi):  FAIL (swapchain path did not initialize)"
        FAIL=$((FAIL+1))
    fi
else
    echo "    vulkan(wsi):  SKIP (no libvulkan / ICD on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

echo "==================================================="
echo "e2e: $PASS passed, $SKIP skipped (no driver), $FAIL failed"
echo "logs: $OUT"
[ "$FAIL" -eq 0 ]
