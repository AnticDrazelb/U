# (04.25) Random A64 programs of load-acquires and store-releases (LDAR, LDARB, LDARH, STLR*, LDAXR/STLXR pairs) with plain loads and stores and DMBs of every type (ISHLD, ISHST, ISH,
# LD, ST, SY) between them, on ordinary and marked pages (the callbacks): for plain_acquire_loads
# (a load-acquire as a mov) and elide_load_store_only_barriers.
#   python3 gen15.py <seed> <out.s>
import random
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
DATA, MARKED = 0x10000000 + 0x1000 * 64, 0x10800000
FREE = list(range(0, 18))
BARRIERS = ["ishld", "ishst", "ish", "ld", "st", "sy", "oshld", "nshst"]


def setreg(reg, value):
    lines = [f"movz {reg}, #{value & 0xFFFF}"]
    for s in (16, 32, 48):
        lines.append(f"movk {reg}, #{(value >> s) & 0xFFFF}, lsl #{s}")
    return lines


def access():
    base = rng.choice(["x22", "x22", "x22", "x23"])
    size = rng.choice(["b", "h", "w", "x"])
    off = {"b": 1, "h": 2, "w": 4, "x": 8}[size] * rng.randrange(0, 64)
    r = "x" if size == "x" else "w"
    t = rng.choice(FREE)
    sfx = {"b": "b", "h": "h", "w": "", "x": ""}[size]
    k = rng.random()
    lines = [f"add x24, {base}, #{off}"]
    if k < 0.3:
        lines += [f"ldar{sfx} {r}{t}, [x24]"]
    elif k < 0.45:
        lines += [f"ldar{sfx} {r}{t}, [x24]", f"add x{rng.choice(FREE)}, x{t}, #1"]
    elif k < 0.65:
        lines += [f"stlr{sfx} {r}{t}, [x24]"]
    elif k < 0.75:
        lines += [f"ldaxr{sfx} {r}{t}, [x24]", f"add {r}{t}, {r}{t}, #1", f"stlxr{sfx} w25, {r}{t}, [x24]",
                  f"add x{rng.choice(FREE)}, x{rng.choice(FREE)}, x25"]
    elif k < 0.88:
        lines += [f"ldr{sfx} {r}{t}, [x24]"]
    else:
        lines += [f"str{sfx} {r}{t}, [x24]"]
    return lines


lines = [".text", ".globl _start", "_start:"]
for x in FREE:
    lines += setreg(f"x{x}", rng.getrandbits(64))
lines += setreg("x22", DATA) + setreg("x23", MARKED)
for _ in range(rng.randint(30, 90)):
    if rng.random() < 0.25:
        lines += [f"dmb {rng.choice(BARRIERS)}"]
    lines += access()
    if rng.random() < 0.3:
        lines += [f"eor x{rng.choice(FREE)}, x{rng.choice(FREE)}, x{rng.choice(FREE)}"]
# the data back into registers, so that the stores show
for i in range(0, 8):
    lines += [f"ldr x{i}, [x22, #{8 * i}]", f"ldr x{i + 8}, [x23, #{8 * i}]"]
lines += ["svc #0"]
open(out, "w").write("\n".join(lines) + "\n")
