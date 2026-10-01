"""Builds the SotC post-process patch from a dumped fs_0x00000000291dbcd0_0.spv.

Usage: python make_patch.py <dumped.spv> <out.spv> [blur_scale] [exposure_scale]
Needs spirv-dis / spirv-as / spirv-val (Vulkan SDK) on PATH or in C:/VulkanSDK/*/Bin.

Edits, located by instruction patterns so they survive recompiler changes:
- the motion blur weight (alpha of the first texture sample, compared against 1.0) is scaled;
- the colour multiplied by the data[37..39] scale before the tone curve is scaled (exposure).
"""
import glob
import os
import re
import subprocess
import sys


def tool(name):
    for d in [""] + sorted(glob.glob("C:/VulkanSDK/*/Bin"), reverse=True):
        path = os.path.join(d, name + ".exe") if d else name
        if not d or os.path.exists(path):
            return path
    return name


def main():
    src, dst = sys.argv[1], sys.argv[2]
    blur = sys.argv[3] if len(sys.argv) > 3 else "0.5"
    exposure = sys.argv[4] if len(sys.argv) > 4 else "0.8"
    asm = subprocess.run([tool("spirv-dis"), "--raw-id", src], capture_output=True, text=True,
                         check=True).stdout
    lines = asm.splitlines()

    def find(pattern, start=0):
        rx = re.compile(pattern)
        for i in range(start, len(lines)):
            m = rx.search(lines[i])
            if m:
                return i, m
        raise SystemExit("pattern not found: " + pattern)

    _, m = find(r"%(\d+) = OpTypeFloat 32")
    f32 = m.group(1)
    _, m = find(r"%(\d+) = OpTypeInt 32 0")
    u32 = m.group(1)
    bound = max(int(x) for x in re.findall(r"%(\d+)", asm)) + 1
    consts = []

    def new_const(value):
        nonlocal bound
        cid = bound
        bound += 1
        consts.append("       %%%d = OpConstant %%%s %s" % (cid, f32, value))
        return cid

    blur_c, exp_c = new_const(blur), new_const(exposure)

    # 1) Motion blur weight: .w of the first image sample.
    i, m = find(r"%(\d+) = OpImageSampleImplicitLod ")
    sample = m.group(1)
    i, m = find(r"^(\s*)%%(\d+) = OpCompositeExtract %%%s %%%s 3$" % (f32, sample), i)
    w = m.group(2)
    tmp = bound
    bound += 1
    lines[i] = "%s%%%d = OpCompositeExtract %%%s %%%s 3" % (m.group(1), tmp, f32, sample)
    lines.insert(i + 1, "%s%%%s = OpFMul %%%s %%%d %%%d" % (m.group(1), w, f32, tmp, blur_c))

    # 2) Exposure: first OpFMul by bitcast(data[37|38|39]).
    for idx in (37, 38, 39):
        _, m = find(r"%%(\d+) = OpConstant %%%s %d$" % (u32, idx))
        cidx = m.group(1)
        _, m = find(r"%%(\d+) = OpAccessChain %%\d+ %%\d+ %%\d+ %%%s$" % cidx)
        chain = m.group(1)
        _, m = find(r"%%(\d+) = OpLoad %%\d+ %%%s$" % chain)
        load = m.group(1)
        _, m = find(r"%%(\d+) = OpBitcast %%%s %%%s$" % (f32, load))
        cast = m.group(1)
        j, m = find(r"^(\s*)%%(\d+) = OpFMul %%%s %%%s %%(\d+)$" % (f32, cast))
        tmp = bound
        bound += 1
        lines[j] = "%s%%%d = OpFMul %%%s %%%s %%%s" % (m.group(1), tmp, f32, cast, m.group(3))
        lines.insert(j + 1, "%s%%%s = OpFMul %%%s %%%d %%%d" % (m.group(1), m.group(2), f32, tmp,
                                                            exp_c))

    # Constants go right after the 32-bit float type declaration.
    i, _ = find(r"%%%s = OpTypeFloat 32" % f32)
    lines[i + 1:i + 1] = consts
    version = re.search(r"; Version: (\d+\.\d+)", asm).group(1)
    out_asm = dst + ".spvasm"
    with open(out_asm, "w") as f:
        f.write("\n".join(lines) + "\n")
    subprocess.run([tool("spirv-as"), "--preserve-numeric-ids", "--target-env", "spv" + version,
                    out_asm, "-o", dst], check=True)
    subprocess.run([tool("spirv-val"), "--target-env", "vulkan1.3", dst], check=True)
    print("patched: blur weight x%s, exposure x%s -> %s" % (blur, exposure, dst))


main()
