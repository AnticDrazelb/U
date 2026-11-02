# Random A32/Thumb programs of exclusive loads and stores (LDREX/STREX, byte, halfword, doubleword,
# CLREX, mismatched and missing pairs, plain stores between), on ordinary, marked and unmapped
# pages, for the inline-exclusive differential test (dynbench test ... vs ... noexcl).
#   python3 gen4.py <seed> <arm|thumb> <out.s>
import random
import sys

seed, mode, out = int(sys.argv[1]), sys.argv[2], sys.argv[3]
rng = random.Random(seed)
DATA, PAGES = 0x10000000, 4096
MARKED, UNMAPPED = 0x10800000, 0x10900000
thumb = mode == "thumb"
SIZES = {"b": 1, "h": 2, "": 4, "d": 8}


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
    return [f"movw r{r}, #{value & 0xFFFF}", f"movt r{r}, #{value >> 16}"]


def pick(exclude, n):
    pool = [r for r in range(0, 13) if r not in exclude]
    return rng.sample(pool, n)


def pair(exclude):
    pool = [r for r in range(0, 12, 2) if r not in exclude and r + 1 not in exclude]
    return rng.choice(pool)


lines = [".syntax unified", ".thumb" if thumb else ".arm", ".text", ".globl _start", "_start:"]
for _ in range(rng.randint(6, 20)):
    sz = rng.choice(["", "", "", "b", "h", "d"])
    base = rng.randrange(0, 13)
    a = addr(SIZES[sz])
    lines += setreg(base, a)
    if sz == "d":
        t = pair({base})
        status = pick({base, t, t + 1}, 1)[0]
        ld = f"ldrexd r{t}, r{t + 1}, [r{base}]"
        modify = [f"adds r{t}, r{t}, #{rng.randrange(1, 255)}", f"adc r{t + 1}, r{t + 1}, #0"]
        st = lambda b=base: f"strexd r{status}, r{t}, r{t + 1}, [r{b}]"
    else:
        t, status = pick({base}, 2)
        ld = f"ldrex{sz} r{t}, [r{base}]"
        modify = [f"add r{t}, r{t}, #{rng.randrange(1, 255)}"]
        st = lambda b=base: f"strex{sz} r{status}, r{t}, [r{b}]"
    kind = rng.random()
    if kind < 0.45:  # the usual read-modify-write
        lines += [ld] + modify + [st()]
    elif kind < 0.55:  # cleared between
        lines += [ld] + modify + ["clrex", st()]
    elif kind < 0.65:  # a store with no load before it (follows whatever state the last one left)
        lines += modify + [st()]
    elif kind < 0.75:  # stored to another granule
        other = pick({base, t, status} | ({t + 1} if sz == "d" else set()), 1)[0]
        lines += setreg(other, (a + 16 * rng.randrange(1, 8)) & ~(SIZES[sz] - 1))
        lines += [ld] + modify + [st(other)]
    elif kind < 0.85:  # a plain store to the same place between (the store fails if it changed the value)
        other = pick({base, t, status} | ({t + 1} if sz == "d" else set()), 1)[0]
        lines += [ld, f"mov r{other}, #{rng.choice([0, rng.randrange(1, 255)])}", f"str{'b' if sz == 'b' else 'h' if sz == 'h' else ''} r{other}, [r{base}]"] + modify + [st()]
    elif kind < 0.92:  # loaded twice, stored once
        lines += [ld, ld] + modify + [st()]
    else:  # stored twice (the second fails)
        lines += [ld] + modify + [st(), st()]
lines += ["svc #0", ""]
open(out, "w").write("\n".join(lines))
