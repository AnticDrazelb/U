import re, sys
f = sys.argv[1]
rows = []
fps = []
for line in open(f, errors="replace"):
    m = re.match(r"\[\s*([\d.]+)\] recompiler: the game function at (\w+) took ([\d.]+) s to compile \(([\d.]+) s of CPU", line)
    if m:
        rows.append((float(m.group(1)), m.group(2), float(m.group(3)), float(m.group(4))))
    m = re.match(r"\[\s*([\d.]+)\] fps ([\d.]+)", line)
    if m:
        fps.append(m.group(2))
wall = sum(r[2] for r in rows)
cpu = sum(r[3] for r in rows)
print("slow compiles %d, wall %.0f s, cpu %.0f s" % (len(rows), wall, cpu))
buckets = {}
for r in rows:
    b = int(r[0] // 60)
    buckets[b] = buckets.get(b, 0) + 1
print("per minute:", " ".join("%dm:%d" % (k, v) for k, v in sorted(buckets.items())))
starved = [r for r in rows if r[3] < r[2] * 0.5]
print("waited more than computed: %d" % len(starved))
print("cpu > 1 s: %d" % len([r for r in rows if r[3] >= 1.0]))
print("fps:", " ".join(fps))
