#!/usr/bin/env python3
# (04.39) The shaders of a GPU hang's suspects (gpu/hang-shaders.txt, written by
# port/mesa/ac_ps4_drm.c's hang report; also inside an eden-ps4-logs.txt bundle), disassembled.
# The PS4's GPU is GCN gfx7 (Bonaire-like), which LLVM can't disassemble; like Mesa's ACO
# (aco_print_asm.cpp) this uses CLRX's clrxdisasm (github.com/CLRX/CLRX-mirror) when it's on the
# PATH, else it writes each shader's bytes to <out>/<stage>-<address>.bin for another tool.
#   gpu-hang-shaders.py <hang-shaders.txt or eden-ps4-logs.txt> [out folder]
import os
import re
import shutil
import struct
import subprocess
import sys


def shaders(lines):
    current = None
    for line in lines:
        if line.startswith("===== ") and current:  # the next file of a bundle
            yield current
            current = None
            continue
        m = re.match(r"==== shader (\w+) (0x[0-9a-f]+) \((\d+) bytes, packet (\d+)\)", line)
        if m:
            if current:
                yield current
            current = {"stage": m[1], "va": int(m[2], 16), "bytes": int(m[3]), "packet": int(m[4]), "words": []}
        elif current and re.match(r"^[0-9a-f]{8}( [0-9a-f]{8})*\s*$", line):
            current["words"] += [int(w, 16) for w in line.split()]
    if current:
        yield current


def main():
    if len(sys.argv) < 2:
        print(__doc__ or "usage: gpu-hang-shaders.py <hang-shaders.txt | eden-ps4-logs.txt> [out]")
        return 1
    with open(sys.argv[1], errors="replace") as f:
        lines = f.readlines()
    # inside a bundle: only the hang-shaders.txt section(s)
    if any(l.startswith("===== ") for l in lines):
        keep, inside = [], False
        for l in lines:
            if l.startswith("===== "):
                inside = "hang-shaders" in l
                keep.append(l)
            elif inside:
                keep.append(l)
        lines = keep
    out = sys.argv[2] if len(sys.argv) > 2 else "hang-shaders"
    os.makedirs(out, exist_ok=True)
    clrx = shutil.which("clrxdisasm")
    count = 0
    for s in shaders(lines):
        count += 1
        data = struct.pack("<%dI" % len(s["words"]), *s["words"])
        name = os.path.join(out, "%s-%x.bin" % (s["stage"], s["va"]))
        with open(name, "wb") as f:
            f.write(data)
        print("==== %s %#x: %d bytes (packet %d) -> %s" % (s["stage"], s["va"], len(data), s["packet"], name))
        if clrx:
            # raw code, GCN 1.1 (Bonaire)
            r = subprocess.run([clrx, "-r", "-g", "bonaire", name], capture_output=True, text=True)
            print(r.stdout if r.returncode == 0 else r.stderr)
    if not count:
        print("no shaders found")
    elif not clrx:
        print("(clrxdisasm isn't on the PATH: only the bytes were written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
