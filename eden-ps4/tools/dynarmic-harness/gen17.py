# (04.28) Random A64 programs for the flags look-ahead's branch targets: a flag setter, then a
# B.cond (every condition), CBZ/CBNZ (every register, W and X) or TBZ/TBNZ (every register and bit)
# whose target starts with a flag reader (ADC, SBC, CSET, CSINC, B.cond, CCMP) while the fall-through
# sets the flags first, or the other way round; the paths joined and repeated. A target off by a
# few bytes (the condition's or the register's bits in its low bits: 04.23-04.27) reads the wrong
# code there and takes the flags for dead.
#   python3 gen17.py <seed> <out.s>
import random
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
REGS = [r for r in range(0, 29) if r not in (18, 22)]
n_label = 0


def label():
    global n_label
    n_label += 1
    return f"L{n_label}"


def setreg(reg, value):
    lines = [f"movz {reg}, #{value & 0xFFFF}"]
    for s in (16, 32, 48):
        lines.append(f"movk {reg}, #{(value >> s) & 0xFFFF}, lsl #{s}")
    return lines


def r():
    return rng.choice(REGS)


def setter():
    a, b = r(), r()
    w = rng.choice("wx")
    return rng.choice([f"cmp {w}{a}, {w}{b}", f"cmn {w}{a}, {w}{b}", f"adds {w}{r()}, {w}{a}, {w}{b}",
                       f"subs {w}{r()}, {w}{a}, #{rng.randrange(0, 4096)}", f"tst {w}{a}, {w}{b}"])


def reader():
    d, a, b = r(), r(), r()
    return rng.choice([f"adc x{d}, x{a}, x{b}", f"sbc w{d}, w{a}, w{b}", f"cset x{d}, {rng.choice(CONDS)}",
                       f"csinc w{d}, w{a}, w{b}, {rng.choice(CONDS)}",
                       f"ccmp x{a}, #{rng.randrange(0, 32)}, #{rng.randrange(0, 16)}, {rng.choice(CONDS)}",
                       f"adcs x{d}, x{a}, x{b}"])


def branch(target):
    k = rng.random()
    reg = r()
    if k < 0.4:
        return f"b.{rng.choice(CONDS)} {target}"
    if k < 0.7:
        return f"{rng.choice(['cbz', 'cbnz'])} {rng.choice('wx')}{reg}, {target}"
    w = rng.choice("wx")
    return f"{rng.choice(['tbz', 'tbnz'])} {w}{reg}, #{rng.randrange(0, 32 if w == 'w' else 64)}, {target}"


def segment():
    target, end = label(), label()
    lines = [setter(), branch(target)]
    if rng.random() < 0.5:
        # the fall-through sets the flags, the target reads them
        lines += [setter(), f"add x{r()}, x{r()}, #1", f"b {end}", f"{target}:", reader(), reader(), f"{end}:"]
    else:
        # the fall-through reads them, the target sets them
        lines += [reader(), f"b {end}", f"{target}:", setter(), reader(), f"{end}:"]
    return lines


lines = [".text", ".globl _start", "_start:"]
for x in REGS:
    lines += setreg(f"x{x}", rng.choice([0, 1, 0xFFFFFFFF, 0x80000000, 0xFFFFFFFFFFFFFFFF]) if rng.random() < 0.4 else rng.getrandbits(64))
lines += ["mov x18, #3", "LOOP:"]
for _ in range(rng.randint(15, 40)):
    lines += segment()
lines += ["subs x18, x18, #1", "b.ne LOOP", "svc #0"]
open(out, "w").write("\n".join(lines) + "\n")
