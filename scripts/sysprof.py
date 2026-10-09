# Usage: python3 sysprof.py samples.log [start seconds]
# system library samples per thread, by module+offset (named from "symbol" lines: the nearest
# kernel entry point at or below), for windows after a start time
import bisect, collections, re, sys
log = sys.argv[1]
start = float(sys.argv[2]) if len(sys.argv) > 2 else 0
modules = []
symbols = {}
per = collections.defaultdict(collections.Counter)
totals = collections.Counter()
cur = None
for line in open(log, errors="replace"):
    m = re.match(r"module (\S+) (.*)", line)
    if m:
        for seg in re.finditer(r"seg (0x[0-9a-f]+)\+(0x[0-9a-f]+)/(\d)", m.group(2)):
            modules.append((int(seg.group(1), 16), int(seg.group(2), 16), m.group(1)))
        continue
    m = re.match(r"symbol (\S+) (0x[0-9a-f]+)", line)
    if m:
        symbols[int(m.group(2), 16)] = m.group(1)
        continue
    m = re.match(r"== ([\d.]+) s, (.+?): (\d+) samples", line)
    if m:
        cur = m.group(2) if float(m.group(1)) >= start else None
        if cur:
            totals[cur] += int(m.group(3))
        continue
    m = re.match(r"\s+([\d.]+)% (jit|sys|elf) (0x[0-9a-f]+)", line)
    if m and cur:
        per[cur][(m.group(2), int(m.group(3), 16))] += float(m.group(1))
addrs = sorted(symbols)
def name(kind, a):
    if kind != "sys":
        return kind
    i = bisect.bisect_right(addrs, a) - 1
    sym = ""
    if i >= 0 and a - addrs[i] < 0x800:
        sym = " %s+0x%x" % (symbols[addrs[i]], a - addrs[i])
    for base, size, mod in modules:
        if base <= a < base + size:
            return "sys %s+0x%x%s" % (mod, a - base, sym)
    return "sys ?%x%s" % (a, sym)
for thread in per:
    if not thread.startswith(("OSSched", "Latte")):
        continue
    c = collections.Counter()
    for (kind, a), v in per[thread].items():
        c[name(kind, a)] += v
    w = sum(c.values())
    print("== %s (%d samples)" % (thread, totals[thread]))
    for k, v in c.most_common(10):
        print("  %5.1f%% %s" % (100 * v / w, k))
