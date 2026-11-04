# (04.23) Random A64 programs of scalar floating point: S and D registers loaded and stored (LDR/STR,
# LDP/STP, LDUR), FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FABS/FNEG/FSQRT, FMADD/FMSUB, FCVT between sizes,
# FMOV to and from W/X, SCVTF/UCVTF/FCVTZS/FCVTZU, FCMP + B.cond and FCSEL, lanes of vectors (MOV
# v.s[n], DUP, INS): for the scalars kept in vector registers (VectorGetElement at lane 0) and the
# quad zero-extension (ZeroExtendLongToQuad of a word).
#   python3 gen12.py <seed> <out.s>
import random
import struct
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
DATA = 0x10000000 + 0x1000 * 64
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
FREE = list(range(0, 18))
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


def fvalue(kind):
    v = rng.choice([0.0, -0.0, 1.0, -1.5, 3.25, 1e-30, 1e30, rng.uniform(-1000, 1000), float('inf'), float('nan')])
    if kind == "s":
        return struct.unpack("<I", struct.pack("<f", v))[0]
    return struct.unpack("<Q", struct.pack("<d", v))[0]


def fr(kind):
    return f"{kind}{rng.randrange(0, 16)}"


def op():
    kind = rng.choice("sd")
    d, n, m, a = fr(kind), fr(kind), fr(kind), fr(kind)
    k = rng.random()
    g = rng.choice(FREE)
    off = rng.randrange(0, 64) * (4 if kind == "s" else 8)
    if k < 0.2:
        return [f"{rng.choice(['fadd', 'fsub', 'fmul', 'fdiv', 'fmax', 'fmin', 'fmaxnm', 'fminnm', 'fnmul'])} {d}, {n}, {m}"]
    if k < 0.3:
        return [f"{rng.choice(['fabs', 'fneg', 'fsqrt', 'fmov', 'frintn', 'frintz'])} {d}, {n}"]
    if k < 0.37:
        return [f"{rng.choice(['fmadd', 'fmsub', 'fnmadd'])} {d}, {n}, {m}, {a}"]
    if k < 0.5:
        return [f"{rng.choice(['ldr', 'str'])} {d}, [x22, #{off}]"]
    if k < 0.55:
        return [f"{rng.choice(['ldur', 'stur'])} {d}, [x22, #{rng.randrange(1, 32) * (4 if kind == 's' else 8) - 3}]"]
    if k < 0.6:
        n2 = f"{kind}{(int(d[1:]) + 1 + rng.randrange(0, 14)) % 16}"
        return [f"{rng.choice(['ldp', 'stp'])} {d}, {n2}, [x22, #{off // 16 * 16}]"]
    if k < 0.65:
        other = "d" if kind == "s" else "s"
        return [f"fcvt {fr(other)}, {n}"]
    if k < 0.72:
        r = ("w" if kind == "s" else "x") + str(g)
        return [f"fmov {r}, {n}"] if rng.random() < 0.5 else [f"fmov {d}, {r}"]
    if k < 0.8:
        r = rng.choice("wx") + str(g)
        if rng.random() < 0.5:
            return [f"{rng.choice(['scvtf', 'ucvtf'])} {d}, {r}"]
        return [f"{rng.choice(['fcvtzs', 'fcvtzu', 'fcvtns', 'fcvtms'])} {r}, {n}"]
    if k < 0.9:
        target, end = label(), label()
        return [f"{rng.choice(['fcmp', 'fcmpe'])} {n}, {m}", f"b.{rng.choice(CONDS)} {target}",
                f"add x{g}, x{g}, #1", f"b {end}", f"{target}:", f"add x{g}, x{g}, #2", f"{end}:"]
    if k < 0.95:
        return [f"fcmp {n}, {m}", f"fcsel {d}, {n}, {m}, {rng.choice(CONDS)}"]
    v = rng.randrange(0, 16)
    lane = rng.randrange(0, 4)
    return [rng.choice([f"mov v{v}.s[{lane}], w{g}", f"dup v{v}.4s, w{g}", f"ins v{v}.s[{lane}], v{rng.randrange(0, 16)}.s[0]",
                        f"mov s{v}, v{rng.randrange(0, 16)}.s[{lane}]", f"fadd v{v}.4s, v{v}.4s, v{rng.randrange(0, 16)}.4s"])]


lines = [".text", ".globl _start", "_start:"]
for x in FREE:
    lines += setreg(f"x{x}", rng.getrandbits(64))
lines += setreg("x22", DATA)
for v in range(16):
    kind = rng.choice("sd")
    lines += setreg("x23", fvalue(kind)) + [f"fmov {'d' if kind == 'd' else 's'}{v}, {'x23' if kind == 'd' else 'w23'}"]
    if rng.random() < 0.5:
        lines += [f"mov v{v}.s[{rng.randrange(1, 4)}], w{rng.choice(FREE)}"]  # garbage in the upper lanes
for i in range(32):
    lines += setreg("x23", fvalue(rng.choice("sd"))) + [f"str x23, [x22, #{i * 8}]"]
for _ in range(rng.randint(30, 90)):
    lines += op()
lines += ["svc #0"]
open(out, "w").write("\n".join(lines) + "\n")
