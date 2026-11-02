# Random A64 programs of exclusive loads and stores (LDXR/STXR, acquire/release forms, byte,
# halfword, word, doubleword, pairs, CLREX, mismatched and missing pairs, plain stores between), on
# ordinary, marked and unmapped pages: dynbench64 test ... vs ... noexcl.
#   python3 gen5.py <seed> <out.s>
import random
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
DATA, PAGES = 0x10000000, 4096
MARKED, UNMAPPED = 0x10800000, 0x10900000
SIZES = {"b": 1, "h": 2, "w": 4, "x": 8, "pw": 8, "px": 16}


def addr(align):
    kind = rng.random()
    if kind < 0.15:
        page = MARKED
    elif kind < 0.22:
        page = UNMAPPED
    elif kind < 0.30:
        page = UNMAPPED + rng.choice([-4096, 4096])
    else:
        page = DATA + 4096 * rng.randrange(16, PAGES - 16)
    off = rng.choice([rng.randrange(0, 64), rng.randrange(4096 - 64, 4096), rng.randrange(0, 4096)])
    return (page + off) & ~(align - 1)


def setreg(r, value):
    return [f"movz x{r}, #{value & 0xFFFF}", f"movk x{r}, #{value >> 16}, lsl #16"]


def pick(exclude, n):
    return rng.sample([r for r in range(0, 29) if r not in exclude], n)


lines = [".text", ".globl _start", "_start:"]
for _ in range(rng.randint(6, 20)):
    sz = rng.choice(["w", "w", "x", "x", "b", "h", "pw", "px"])
    base = rng.randrange(0, 29)
    a = addr(SIZES[sz])
    lines += setreg(base, a)
    acq = rng.choice(["", "a"])
    rel = rng.choice(["", "l"])
    if sz.startswith("p"):
        r = "w" if sz == "pw" else "x"
        t, t2, status = pick({base}, 3)
        ld = f"ld{acq}xp {r}{t}, {r}{t2}, [x{base}]"
        modify = [f"add {r}{t}, {r}{t}, #{rng.randrange(1, 255)}"]
        st = lambda b=base: f"st{rel}xp w{status}, {r}{t}, {r}{t2}, [x{b}]"
        used = {base, t, t2, status}
    else:
        r = "x" if sz == "x" else "w"
        suffix = sz if sz in ("b", "h") else ""
        t, status = pick({base}, 2)
        ld = f"ld{acq}xr{suffix} {r}{t}, [x{base}]"
        modify = [f"add {r}{t}, {r}{t}, #{rng.randrange(1, 255)}"]
        st = lambda b=base: f"st{rel}xr{suffix} w{status}, {r}{t}, [x{b}]"
        used = {base, t, status}
    kind = rng.random()
    if kind < 0.45:
        lines += [ld] + modify + [st()]
    elif kind < 0.55:
        lines += [ld] + modify + ["clrex", st()]
    elif kind < 0.65:
        lines += modify + [st()]
    elif kind < 0.75:
        other = pick(used, 1)[0]
        lines += setreg(other, (a + 16 * rng.randrange(1, 8)) & ~(SIZES[sz] - 1))
        lines += [ld] + modify + [st(other)]
    elif kind < 0.85:
        other = pick(used, 1)[0]
        store = {"b": "strb", "h": "strh"}.get(sz, "str")
        lines += [ld, f"mov w{other}, #{rng.choice([0, rng.randrange(1, 255)])}", f"{store} w{other}, [x{base}]"] + modify + [st()]
    elif kind < 0.92:
        lines += [ld, ld] + modify + [st()]
    else:
        lines += [ld] + modify + [st(), st()]
lines += ["svc #0", ""]
open(out, "w").write("\n".join(lines))
