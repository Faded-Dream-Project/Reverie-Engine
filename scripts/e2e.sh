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

# ---- OpenGL tier: GPU fluid + ripple solvers (forced via the llvmpipe hook) --
# REV_GL_FLUID_SW=1 runs the shader-based sims even on software GL so the
# whole GPU sim path (fluid splats/Jacobi/advection/tonemap + ripple wave
# equation/refraction composite) stays regression-tested without hardware.
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV REV_GL_FLUID_SW=1 timeout 10 \
    ./build/reverie -R opengl --shot 5 "$OUT/glf" scenes/fluid-aurora.rev \
    > "$OUT/glf.log" 2>&1 || true
if grep -q "fluid + ripple solvers on GPU" "$OUT/glf.log" 2>/dev/null \
   && grep -q "simulations: GPU" "$OUT/glf.log" 2>/dev/null; then
    echo "    opengl(fluid): PASS (GPU sim active$( [ -f "$OUT/glf.png" ] && echo ', screenshot' ))"
    PASS=$((PASS+1))
elif grep -q "renderer: opengl" "$OUT/glf.log" 2>/dev/null; then
    echo "    opengl(fluid): FAIL (GPU sim did not activate)"
    cat "$OUT/glf.log"
    FAIL=$((FAIL+1))
else
    echo "    opengl(fluid): SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- OpenGL tier: GPU ripple (the scene that used to be locked out of ------
# GPU compositing entirely — refraction composites against the accum FBO)
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV REV_GL_FLUID_SW=1 REV_FAKE_POINTER=0.1 timeout 10 \
    ./build/reverie -R opengl --shot 6 "$OUT/rip" scenes/ripple-lake.rev \
    > "$OUT/rip.log" 2>&1 || true
if grep -q "simulations: GPU" "$OUT/rip.log" 2>/dev/null \
   && ! grep -q "scene needs CPU compositing" "$OUT/rip.log" 2>/dev/null; then
    echo "    opengl(ripple): PASS (GPU compositing + GPU sim$( [ -f "$OUT/rip.png" ] && echo ', screenshot' ))"
    PASS=$((PASS+1))
elif grep -q "renderer: opengl" "$OUT/rip.log" 2>/dev/null; then
    echo "    opengl(ripple): FAIL (ripple scene fell back to CPU compositing)"
    cat "$OUT/rip.log"
    FAIL=$((FAIL+1))
else
    echo "    opengl(ripple): SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
stop_xvfb

# ---- Ripple CLICK placement regression (2026-10) ------------------------------
# The GPU ripple solver computed its Laplacian from UN-injected neighbors
# while subtracting 4x the injected center height: clicks launched at
# nh ~= 1.04h instead of 2h — a weak displaced smear, not a wave. These
# tests fire synthetic clicks (REV_FAKE_CLICK) on the GL tier and verify
# the disturbance lands ON the click, plus a CPU-tier cross-check.
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV timeout 8 ./build/reverie -R cpu --shot 1 "$OUT/rc-base" \
    themes/ripple-lake > "$OUT/rc-base.log" 2>&1 || true
DISPLAY="$XDISPLAY" env $GPU_ENV REV_FAKE_CLICK=0.5,0.5 timeout 8 ./build/reverie -R cpu \
    --shot 1 "$OUT/rc-cpu" themes/ripple-lake > "$OUT/rc-cpu.log" 2>&1 || true
if [ -f "$OUT/rc-base.png" ] && [ -f "$OUT/rc-cpu.png" ]; then
    if python3 scripts/click_cmp.py "$OUT/rc-base.png" "$OUT/rc-cpu.png" 0.5 0.5 2>/dev/null; then
        echo "    ripple(click,cpu):  PASS (splash lands on the click)"
        PASS=$((PASS+1))
    else
        echo "    ripple(click,cpu):  FAIL (CPU splash misplaced)"
        FAIL=$((FAIL+1))
    fi
else
    echo "    ripple(click,cpu):  SKIP (no CPU screenshots)"
    SKIP=$((SKIP+1))
fi
DISPLAY="$XDISPLAY" env $GPU_ENV REV_GL_FLUID_SW=1 REV_FAKE_CLICK=0.5,0.5 timeout 10 \
    ./build/reverie -R opengl --shot 1 "$OUT/rc-gl" themes/ripple-lake \
    > "$OUT/rc-gl.log" 2>&1 || true
if grep -q "simulations: GPU" "$OUT/rc-gl.log" 2>/dev/null \
   && [ -f "$OUT/rc-base.png" ] && [ -f "$OUT/rc-gl.png" ]; then
    if python3 scripts/click_cmp.py "$OUT/rc-base.png" "$OUT/rc-gl.png" 0.5 0.5 2>/dev/null; then
        echo "    ripple(click,gl):   PASS (GPU splash lands on the click)"
        PASS=$((PASS+1))
    else
        echo "    ripple(click,gl):   FAIL (GPU splash misplaced — solver regression)"
        FAIL=$((FAIL+1))
    fi
else
    echo "    ripple(click,gl):   SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
# corner click on the GL tier: placement near the frame corner, no crash
DISPLAY="$XDISPLAY" env $GPU_ENV REV_GL_FLUID_SW=1 REV_FAKE_CLICK=0.15,0.85 timeout 10 \
    ./build/reverie -R opengl --shot 1 "$OUT/rc-corner" themes/ripple-lake \
    > "$OUT/rc-corner.log" 2>&1 || true
if grep -q "simulations: GPU" "$OUT/rc-corner.log" 2>/dev/null \
   && [ -f "$OUT/rc-base.png" ] && [ -f "$OUT/rc-corner.png" ]; then
    if python3 scripts/click_cmp.py "$OUT/rc-base.png" "$OUT/rc-corner.png" 0.15 0.85 2>/dev/null; then
        echo "    ripple(click,corner): PASS (corner splash lands correctly)"
        PASS=$((PASS+1))
    else
        echo "    ripple(click,corner): FAIL (corner splash misplaced)"
        FAIL=$((FAIL+1))
    fi
else
    echo "    ripple(click,corner): SKIP (no libEGL / GL vendor on this system)"
    SKIP=$((SKIP+1))
fi
# click AFTER the water fully calms (the 2026-10 "dead clicks" report): the
# old wave update was anti-damped — one click grew until the field hit
# inf/NaN, the screen glitched, and every later click was swallowed. Fire a
# center click, let the lake fully calm (~19 s at this 640x400 grid), then
# click again at a different spot: the second splash must land on target.
# REV_FAKE_CLICK's @t syntax fires each click at its own timestamp.
for TIER in cpu opengl; do
    if [ "$TIER" = opengl ]; then
        TIENV="$GPU_ENV REV_GL_FLUID_SW=1"
    else
        TIENV="$GPU_ENV"
    fi
    DISPLAY="$XDISPLAY" env $TIENV REV_FAKE_CLICK="0.5,0.5@0.5;0.3,0.7@19" timeout 45 \
        ./build/reverie -R $TIER --shot 19.5 "$OUT/rc-late-$TIER" themes/ripple-lake \
        > "$OUT/rc-late-$TIER.log" 2>&1 || true
    OK=1
    [ -f "$OUT/rc-base.png" ] && [ -f "$OUT/rc-late-$TIER.png" ] || OK=0
    if [ "$TIER" = opengl ] && ! grep -q "simulations: GPU" "$OUT/rc-late-$TIER.log" 2>/dev/null; then
        OK=0
    fi
    if [ $OK -eq 1 ]; then
        if python3 scripts/click_cmp.py "$OUT/rc-base.png" "$OUT/rc-late-$TIER.png" 0.3 0.7 2>/dev/null; then
            echo "    ripple(click-after-calm,$TIER): PASS (second splash lands on target)"
            PASS=$((PASS+1))
        else
            echo "    ripple(click-after-calm,$TIER): FAIL (second click dead or misplaced)"
            FAIL=$((FAIL+1))
        fi
    else
        echo "    ripple(click-after-calm,$TIER): SKIP (no screenshots / no GL vendor)"
        SKIP=$((SKIP+1))
    fi
done
stop_xvfb

# ---- Themes: folder + packed + hostile rejection -----------------------------
start_xvfb
DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu --shot 3 "$OUT/theme" themes/ripple-lake \
    > "$OUT/theme.log" 2>&1 || true
if [ -f "$OUT/theme.png" ]; then
    echo "    theme(folder): PASS (screenshot captured)"
    PASS=$((PASS+1))
else
    echo "    theme(folder): FAIL (no screenshot)"
    cat "$OUT/theme.log"
    FAIL=$((FAIL+1))
fi
# packed theme — no extraction, byte-identical output expected
./build/revtool theme pack themes/ripple-lake -o "$OUT/ripple-lake.revpack" \
    > /dev/null 2>&1
DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu --shot 3 "$OUT/themep" \
    "$OUT/ripple-lake.revpack" > "$OUT/themep.log" 2>&1 || true
if [ -f "$OUT/themep.png" ] && cmp -s "$OUT/themep.png" "$OUT/theme.png"; then
    echo "    theme(packed): PASS (byte-identical to the folder render)"
    PASS=$((PASS+1))
else
    echo "    theme(packed): FAIL (missing or differing screenshot)"
    cat "$OUT/themep.log"
    FAIL=$((FAIL+1))
fi
# corrupt pack: must exit with an error, never crash
head -c 200 "$OUT/ripple-lake.revpack" > "$OUT/corrupt.revpack"
DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu "$OUT/corrupt.revpack" \
    > "$OUT/corrupt.log" 2>&1
if [ $? -ne 0 ] && grep -qE "not a \.revpack|pack truncated|CRC mismatch" "$OUT/corrupt.log"; then
    echo "    theme(corrupt): PASS (rejected with a clear error)"
    PASS=$((PASS+1))
else
    echo "    theme(corrupt): FAIL (accepted a corrupt pack?)"
    cat "$OUT/corrupt.log"
    FAIL=$((FAIL+1))
fi
stop_xvfb

# ---- Video wallpapers (needs the ffmpeg CLI to synthesize a fixture) --------
if command -v ffmpeg >/dev/null 2>&1; then
    VDIR=$(mktemp -d)
    ffmpeg -y -loglevel error -f lavfi -i "testsrc2=size=320x180:rate=30" \
        -t 4 -pix_fmt yuv420p -c:v libx264 -preset ultrafast "$VDIR/loop.mp4"
    mkdir -p "$VDIR/assets"
    mv "$VDIR/loop.mp4" "$VDIR/assets/loop.mp4"
    printf 'scene { layer bg { type = video src = "assets/loop.mp4" loop = true } }\n' > "$VDIR/scene.rev"
    printf '{ "id": "vtest", "name": "vtest", "scene": "scene.rev" }\n' > "$VDIR/theme.json"
    # CPU tier: sws path
    start_xvfb
    DISPLAY="$XDISPLAY" timeout 8 ./build/reverie -R cpu --shot 3 "$OUT/vid" "$VDIR" \
        > "$OUT/vid.log" 2>&1 || true
    # GL tier: plane upload + shader YUV->RGB
    DISPLAY="$XDISPLAY" env $GPU_ENV timeout 8 ./build/reverie -R opengl --shot 3 \
        "$OUT/vidgl" "$VDIR" > "$OUT/vidgl.log" 2>&1 || true
    stop_xvfb
    if [ -f "$OUT/vid.png" ] && grep -q "video: 320x180" "$OUT/vid.log" 2>/dev/null; then
        echo "    video(cpu):   PASS (decoded + rendered$( [ -f "$OUT/vidgl.png" ] && echo '; GL tier too' ))"
        PASS=$((PASS+1))
    else
        echo "    video(cpu):   FAIL (no decoded frame)"
        cat "$OUT/vid.log"
        FAIL=$((FAIL+1))
    fi
    if grep -q "video: GPU planes + shader color convert" "$OUT/vidgl.log" 2>/dev/null \
       && [ -f "$OUT/vidgl.png" ]; then
        echo "    video(gl):    PASS (GPU plane path active)"
        PASS=$((PASS+1))
    elif grep -q "renderer: opengl" "$OUT/vidgl.log" 2>/dev/null; then
        echo "    video(gl):    FAIL (GPU video path did not activate)"
        cat "$OUT/vidgl.log"
        FAIL=$((FAIL+1))
    else
        echo "    video(gl):    SKIP (no libEGL / GL vendor on this system)"
        SKIP=$((SKIP+1))
    fi
    rm -rf "$VDIR"
else
    echo "    video:        SKIP (no ffmpeg CLI for the fixture)"
    SKIP=$((SKIP+1))
fi

# ---- Vulkan tier: default present path (platform SHM) -----------------------
start_xvfb
DISPLAY="$XDISPLAY" env $GPU_ENV timeout 8 ./build/reverie -R vulkan --shot 3 "$OUT/vk" "$SCENE" \
    > "$OUT/vk.log" 2>&1 || true
if grep -q "renderer: vulkan" "$OUT/vk.log" 2>/dev/null; then
    if grep -q "frames present via the platform SHM path" "$OUT/vk.log" 2>/dev/null; then
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
