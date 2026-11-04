# (04.26) Random A64 programs of sign extensions: LDRSB/LDRSH/LDRSW into W and X (plain, unscaled,
# register offset), SXTB/SXTH/SXTW of registers just written by 32-bit ops, loads or nothing, the
# sign-extended values extended again (LDRSW then SXTW), SMULL/SMADDL/SMSUBL of W operands, ADD/SUB
# with SXTW/SXTH/SXTB-extended registers, on edge values (the sign bits of each size): for the
# sign-extending loads (movsx/movsxd from memory), W reads sign-extended from the JIT state, and the
# second SXTW of a sign-extended value dropped.
#   python3 gen16.py <seed> <out.s>
import random
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
DATA = 0x10000000 + 0x1000 * 64
FREE = list(range(0, 18))
EDGES = [0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
         0xFFFFFFFFFFFFFFFF, 0x8000000000000000, 0x123456789ABCDEF0, 0xFFFFFFFF80000000]


def setreg(reg, value):
    lines = [f"movz {reg}, #{value & 0xFFFF}"]
    for s in (16, 32, 48):
        lines.append(f"movk {reg}, #{(value >> s) & 0xFFFF}, lsl #{s}")
    return lines


def r():
    return rng.choice(FREE)


def step():
    k = rng.random()
    d, a, b, c = r(), r(), r(), r()
    if k < 0.25:
        op, sz = rng.choice([("ldrsw", 4), ("ldrsh", 2), ("ldrsb", 1)])
        w = "x" if op == "ldrsw" else rng.choice("wx")
        form = rng.random()
        if form < 0.5:
            return [f"{op} {w}{d}, [x22, #{sz * rng.randrange(0, 64)}]"]
        if form < 0.75:
            op_u = op.replace("ldr", "ldur")
            return [f"{op_u} {w}{d}, [x22, #{rng.randrange(-32, 200)}]"]
        return [f"and x24, x{a}, #0x3f", f"{op} {w}{d}, [x22, x24, lsl #{ {4: 2, 2: 1, 1: 0}[sz] }]"]
    if k < 0.45:
        ext = rng.choice(["sxtw", "sxth", "sxtb"])
        src = rng.choice(["", "add", "ldr"])
        lines = []
        if src == "add":
            lines.append(f"add w{a}, w{a}, w{b}")
        elif src == "ldr":
            lines.append(f"ldrsw x{a}, [x22, #{4 * rng.randrange(0, 64)}]")
        lines.append(f"{ext} x{d}, w{a}")
        if rng.random() < 0.4:
            lines.append(f"sxtw x{c}, w{d}")  # (sign-extended again)
        return lines
    if k < 0.65:
        op = rng.choice(["smull", "smaddl", "smsubl", "umull"])
        if op in ("smull", "umull"):
            return [f"{op} x{d}, w{a}, w{b}"]
        return [f"{op} x{d}, w{a}, w{b}, x{c}"]
    if k < 0.8:
        ext = rng.choice(["sxtw", "sxth", "sxtb", "uxtw"])
        return [f"{rng.choice(['add', 'sub'])} x{d}, x{a}, w{b}, {ext} #{rng.randrange(0, 5)}"]
    if k < 0.9:
        return setreg(f"x{d}", rng.choice(EDGES))
    return [f"str x{d}, [x22, #{8 * rng.randrange(0, 64)}]", f"str w{a}, [x22, #{4 * rng.randrange(128, 192)}]"]


lines = [".text", ".globl _start", "_start:"]
for x in FREE:
    lines += setreg(f"x{x}", rng.choice(EDGES) if rng.random() < 0.6 else rng.getrandbits(64))
lines += setreg("x22", DATA)
# the data: edge values
for i in range(64):
    lines += setreg("x23", rng.choice(EDGES)) + [f"str x23, [x22, #{8 * i}]"]
for _ in range(rng.randint(40, 120)):
    lines += step()
lines += ["svc #0"]
open(out, "w").write("\n".join(lines) + "\n")
