#!/bin/sh
# ============================================================================
# Reverie Engine — scripts/gen_spv.sh
#
# Regenerates src/vk_spv.h (the embedded SPIR-V compute kernels) from
# src/shaders/*.comp. Requires glslangValidator (Debian: glslang-tools).
#
# The generated header is COMMITTED to the repository — a normal build
# never runs this script and never needs a shader compiler. Run it only
# when a kernel changes; then commit both the .comp and the new vk_spv.h.
#
# Kernels are compiled for the vulkan1.0 target environment (core
# Vulkan 1.0: compute pipelines, storage images, dynamic uniform buffers,
# timestamp queries — no extensions, no device features).
# ============================================================================
cd "$(dirname "$0")/.." || exit 1

GLSLANG="${GLSLANG:-}"
if [ -z "$GLSLANG" ]; then
    if command -v glslangValidator >/dev/null 2>&1; then
        GLSLANG=glslangValidator
    elif [ -x "../gpulibs/spirv/root/usr/bin/glslangValidator" ]; then
        GLSLANG="../gpulibs/spirv/root/usr/bin/glslangValidator"
    else
        echo "gen_spv: glslangValidator not found (apt install glslang-tools)" >&2
        exit 1
    fi
fi

OUT=build/spv
mkdir -p "$OUT"

KERN="composite copypremul blur gradient particles video decode \
ripple_splat ripple_step ripple_composite \
fluid_velf fluid_dyef fluid_curl fluid_vort fluid_div fluid_jacobi \
fluid_gradsub fluid_advect_vel fluid_advect_dye fluid_tonemap"

for k in $KERN; do
    if [ ! -f "src/shaders/$k.comp" ]; then
        echo "gen_spv: missing src/shaders/$k.comp" >&2
        exit 1
    fi
    if ! "$GLSLANG" -V --target-env vulkan1.0 -o "$OUT/$k.spv" "src/shaders/$k.comp"; then
        echo "gen_spv: $k failed to compile" >&2
        exit 1
    fi
done

python3 - "$OUT" src/vk_spv.h $KERN <<'PYEOF'
import sys, struct
outdir, header, kernels = sys.argv[1], sys.argv[2], sys.argv[3:]
lines = []
lines.append("/* ============================================================================")
lines.append(" * Reverie Engine — vk_spv.h  [GENERATED — do not edit by hand]")
lines.append(" *")
lines.append(" * Embedded SPIR-V compute kernels for the Vulkan renderer, compiled from")
lines.append(" * the .comp sources under src/shaders, by scripts/gen_spv.sh")
lines.append(" * --target-env vulkan1.0). Regenerate + commit together with the .comp")
lines.append(" * sources; a normal build only needs this header.")
lines.append(" *")
lines.append(" * Proprietary — see LICENSE. Copyright (c) 2026 FemBoyGamerTechGuy.")
lines.append(" * ============================================================================*/")
lines.append("#ifndef REV_VK_SPV_H")
lines.append("#define REV_VK_SPV_H")
lines.append("")
names = []
for k in kernels:
    data = open(f"{outdir}/{k}.spv", "rb").read()
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    assert len(data) % 4 == 0
    names.append((k, len(words)))
    lines.append(f"static const unsigned int spv_{k}[{len(words)}] = {{")
    for i in range(0, len(words), 6):
        row = ", ".join("0x%08xu" % w for w in words[i:i+6])
        lines.append(f"        {row},")
    lines.append("};")
    lines.append("")
lines.append("enum {")
for k, n in names:
    lines.append(f"        SPV_{k.upper()}_{n},")
lines.append("};")
lines.append("")
lines.append("#endif /* REV_VK_SPV_H */")
open(header, "w").write("\n".join(lines) + "\n")
print(f"gen_spv: wrote {header} ({len(kernels)} kernels)")
for k, n in names:
    print(f"  {k}: {n} words ({n*4} bytes)")
PYEOF
