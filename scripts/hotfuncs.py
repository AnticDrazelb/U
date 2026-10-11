# The busiest recompiled game functions (Crema's profiler, hotfuncs.bin) with their code: what the
# game does there (PowerPC) and what the recompiler made of it (x86), with the profiler's samples
# per 16 bytes of x86 code from samples.log ("jit ... ppc <function> +<offset>" lines).
#   python3 hotfuncs.py hotfuncs.bin samples.log [functions] [thread filter, e.g. "core=2"]
import collections
import re
import struct
import subprocess
import sys
import tempfile

data = open(sys.argv[1], "rb").read()
samples_log = sys.argv[2] if len(sys.argv) > 2 else None
count = int(sys.argv[3]) if len(sys.argv) > 3 else 10
thread_filter = sys.argv[4] if len(sys.argv) > 4 else None

funcs = []
at = 0
while at + 28 <= len(data):
    magic, ppc, ppc_size, x86_size, samples = struct.unpack_from("<5I", data, at)
    if magic != 0x434E4648:
        break
    (x86,) = struct.unpack_from("<Q", data, at + 20)
    at += 28
    code = data[at:at + ppc_size]
    at += ppc_size
    x86_code = data[at:at + x86_size]
    at += x86_size
    funcs.append(dict(ppc=ppc, ppc_size=ppc_size, x86=x86, x86_size=x86_size, samples=samples, code=code, x86_code=x86_code))

# samples per (function, 16-byte x86 bucket), from the thread windows that match the filter
buckets = collections.defaultdict(collections.Counter)
totals = collections.Counter()
if samples_log:
    keep = True
    for line in open(samples_log, errors="replace"):
        m = re.match(r"== [\d.]+ s, (.+?): (\d+) samples", line)
        if m:
            keep = thread_filter is None or thread_filter in m.group(1)
            continue
        m = re.match(r"\s+([\d.]+)% jit 0x[0-9a-f]+ ppc ([0-9a-f]{8}) \+([0-9a-f]+)", line)
        if m and keep:
            buckets[int(m.group(2), 16)][int(m.group(3), 16) & ~0xF] += float(m.group(1))
            totals[int(m.group(2), 16)] += float(m.group(1))


def ppc_disasm(base, code):
    words = len(code) // 4
    body = code[:words * 4]
    names = b"\0.shstrtab\0.text\0"
    strtab = 0x34 + len(body)
    blob = body + names + b"\0" * (-len(names) % 4)
    shoff = 0x34 + len(blob)
    sections = [struct.pack(">10I", *([0] * 10)), struct.pack(">10I", 1, 3, 0, 0, strtab, len(names), 0, 0, 1, 0),
                struct.pack(">10I", 11, 1, 6, base, 0x34, len(body), 0, 0, 4, 0)]
    header = b"\x7fELF\x01\x02\x01" + b"\0" * 9 + struct.pack(">HHIIIIIHHHHHH", 2, 20, 1, 0, 0, shoff, 0, 0x34, 0, 0, 40, 3, 1)
    with tempfile.NamedTemporaryFile(suffix=".elf", delete=False) as f:
        f.write(header + blob + b"".join(sections))
    out = subprocess.run(["llvm-objdump-21", "-d", "--no-show-raw-insn", "--triple=powerpc", f.name], capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if re.match(r"\s*[0-9a-f]+:", l)]


def x86_disasm(code):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(code)
    subprocess.run(["llvm-objcopy-21", "-I", "binary", "-O", "elf64-x86-64", "--rename-section", ".data=.text,code", f.name, f.name + ".o"], check=True)
    out = subprocess.run(["llvm-objdump-21", "-d", "--no-show-raw-insn", f.name + ".o"], capture_output=True, text=True).stdout
    result = []
    for l in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s*(.*)", l)
        if m:
            result.append((int(m.group(1), 16), m.group(2)))
    return result


# PowerPC instruction kinds, by mnemonic
def kind(mnemonic):
    if mnemonic.startswith(("ps_", "psq_")):
        return "paired single"
    if mnemonic.startswith(("lf", "stf")):
        return "float load/store"
    if mnemonic.startswith("f"):
        return "float"
    if mnemonic.startswith(("l", "st")) and not mnemonic.startswith(("li", "lis")):
        return "load/store"
    if mnemonic.startswith("b"):
        return "branch"
    if mnemonic.startswith(("cmp", "cr", "mfcr", "mtcr")):
        return "compare/cr"
    return "integer"


order = sorted(funcs, key=lambda f: -(totals.get(f["ppc"], 0) if totals else f["samples"]))
print("%-10s %8s %7s %7s %6s  %s" % ("function", "samples", "ppc B", "x86 B", "x86/ppc", "PowerPC mix"))
for f in order:
    insns = ppc_disasm(f["ppc"], f["code"])
    mix = collections.Counter(kind(l.split()[1]) for l in insns if len(l.split()) > 1)
    total = sum(mix.values()) or 1
    mix_text = ", ".join("%s %d%%" % (k, 100 * v // total) for k, v in mix.most_common(4))
    f["insns"] = insns
    print("%08x   %8s %7d %7d %6.1f  %s" % (f["ppc"], "%.1f%%" % totals[f["ppc"]] if totals else f["samples"], f["ppc_size"],
                                         f["x86_size"], f["x86_size"] / max(f["ppc_size"], 1), mix_text))

for f in order[:count]:
    print("\n==== %08x (%d bytes of PowerPC, %d of x86)" % (f["ppc"], f["ppc_size"], f["x86_size"]))
    hot = buckets.get(f["ppc"], {})
    if hot:
        print("-- hottest x86 (samples per 16 bytes):")
        x86 = x86_disasm(f["x86_code"])
        for bucket, share in sorted(hot.items(), key=lambda kv: -kv[1])[:6]:
            print("  +%05x  %.2f%%" % (bucket, share))
            for offset, text in x86:
                if bucket - 0x20 <= offset < bucket + 0x30:
                    print("      %s+%05x: %s" % (">" if bucket <= offset < bucket + 16 else " ", offset, text))
    print("-- PowerPC:")
    for l in f["insns"][:400]:
        print("  " + l.strip())
