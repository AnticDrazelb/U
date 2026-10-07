# Reads the profiler's samples.log (port/ps4_sampler.cpp; Crema: Diagnostics > Profiler) and
# prints, per sampled thread, where its time went over the whole run: eboot functions (named by
# llvm-symbolizer from the build's ELF), translated game code ("JIT") and system libraries.
#   python3 profile.py samples.log app.elf [top]
import collections
import re
import subprocess
import sys

log, elf = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25

threads = collections.OrderedDict()  # name -> Counter of keys, total samples
current = None
for line in open(log, errors="replace"):
    m = re.match(r"== ([\d.]+) s, (.+?): (\d+) samples", line)
    if m:
        current = m.group(2)
        entry = threads.setdefault(current, [collections.Counter(), 0])
        entry[1] += int(m.group(3))
        continue
    m = re.match(r"\s+([\d.]+)% (jit|sys|elf) (0x[0-9a-f]+)", line)
    if m and current:
        # the window's percentage back to a sample count
        entry = threads[current]
        kind, address = m.group(2), m.group(3)
        threads[current][0][(kind, address)] += float(m.group(1))

# name every eboot address once
addresses = sorted({a for t in threads.values() for (k, a) in t[0] if k == "elf"})
names = {}
if addresses:
    out = subprocess.run(["llvm-symbolizer-21", "--obj=" + elf, "--functions=linkage", "--demangle", "--no-inlines"],
                         input="\n".join(addresses), capture_output=True, text=True).stdout
    blocks = [b.split("\n") for b in out.strip().split("\n\n")]
    for address, block in zip(addresses, blocks):
        names[address] = block[0] if block and block[0] else "??"

for name, (counter, total) in threads.items():
    functions = collections.Counter()
    for (kind, address), percent in counter.items():
        key = names.get(address, "??") if kind == "elf" else ("[translated game code]" if kind == "jit" else "[system library]")
        functions[re.sub(r"\(.*", "", key)[:110]] += percent
    weight = sum(functions.values())
    print("== %s: %d samples" % (name, total))
    for function, percent in functions.most_common(top):
        print("  %5.1f%%  %s" % (100.0 * percent / weight, function))
    print()
