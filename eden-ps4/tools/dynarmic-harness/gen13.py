# (04.23) Random A64 programs of conditional selects after what set their flags, in the same block:
# every flag setter (CMP/CMN/SUBS/ADDS/TST/ANDS/BICS, W and X, register, shifted, immediate; FCMP,
# FCMPE on S and D, with zero) followed by one to three CSEL/CSINC/CSINV/CSNEG/CSET/CSETM/CINC/CNEG
# or FCSEL on every condition (selects of the same flags in a row, a setter's own result selected,
# XZR/WZR operands), on edge values; then a branch on the same flags or other code: for
# A64SelectOnHostFlagsPass (cmp; cmov) and the dead flags around it.
#   python3 gen13.py <seed> <out.s>
import random
import struct
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
DATA = 0x10000000 + 0x1000 * 64
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al"]
EDGES = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000,
         0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFF7FFFFFFF, 0xFFFFFFFF80000000, 4095, 4096]
FLOATS = [0.0, -0.0, 1.0, -1.0, 2.5, float("inf"), float("-inf"), float("nan"), 1e-40, 3.0e38]
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


def value():
    return rng.choice(EDGES) if rng.random() < 0.6 else rng.getrandbits(64)


def r(w):
    return f"{w}{rng.choice(FREE)}" if rng.random() < 0.85 else f"{w}zr"


def setter(w, a, b):
    n, m = f"{w}{a}", f"{w}{b}"
    bits = 32 if w == "w" else 64
    d = f"{w}{rng.choice(FREE)}"
    k = rng.random()
    if k < 0.25:
        return [f"{rng.choice(['cmp', 'cmn', 'tst'])} {n}, {m}"], d
    if k < 0.45:
        return [f"{rng.choice(['subs', 'adds', 'ands', 'bics'])} {d}, {n}, {m}"], d
    if k < 0.65:
        op = rng.choice(["cmp", "cmn", "subs", "adds"])
        imm = rng.choice([0, 1, 4095, rng.randrange(0, 4096)])
        return ([f"{op} {n}, #{imm}"] if op in ("cmp", "cmn") else [f"{op} {d}, {n}, #{imm}"]), d
    if k < 0.8:
        op = rng.choice(["cmp", "cmn", "subs", "adds"])
        sh = f"{rng.choice(['lsl', 'lsr', 'asr'])} #{rng.randrange(0, bits)}"
        return ([f"{op} {n}, {m}, {sh}"] if op in ("cmp", "cmn") else [f"{op} {d}, {n}, {m}, {sh}"]), d
    if k < 0.88:
        return [f"{rng.choice(['adcs', 'sbcs'])} {d}, {n}, {m}"], d
    imm = rng.choice([1, 0xff, 0x80, 0xf0, 0x80000000 if w == "w" else 0x8000000000000000])
    if rng.random() < 0.5:
        return [f"tst {n}, #{imm}"], d
    return [f"ands {d}, {n}, #{imm}"], d


def select(w, own):
    c = rng.choice(CONDS[:-1] if rng.random() < 0.9 else CONDS)
    d = f"{w}{rng.choice(FREE)}"
    a = w + own[1:] if own and rng.random() < 0.3 else r(w)
    b = r(w)
    k = rng.random()
    if c == "al" and k >= 0.6:
        c = "eq"
    if k < 0.3:
        return f"csel {d}, {a}, {b}, {c}"
    if k < 0.4:
        return f"csinc {d}, {a}, {b}, {c}"
    if k < 0.5:
        return f"csinv {d}, {a}, {b}, {c}"
    if k < 0.6:
        return f"csneg {d}, {a}, {b}, {c}"
    if k < 0.75:
        return f"cset {d}, {c}"
    if k < 0.85:
        return f"csetm {d}, {c}"
    if k < 0.93:
        return f"cinc {d}, {a}, {c}"
    return f"cneg {d}, {a}, {c}"


def fsetter():
    t = rng.choice("sd")
    a, b = rng.randrange(0, 8), rng.randrange(0, 8)
    op = rng.choice(["fcmp", "fcmpe"])
    if rng.random() < 0.2:
        return [f"{op} {t}{a}, #0.0"], t
    return [f"{op} {t}{a}, {t}{b}"], t


def segment():
    lines = []
    a, b = rng.choice(FREE), rng.choice(FREE)
    if rng.random() < 0.7:
        lines += setreg(f"x{a}", value())
        lines += setreg(f"x{b}", value())
    if rng.random() < 0.2:
        set_lines, t = fsetter()
        lines += set_lines
        for _ in range(rng.randint(1, 3)):
            if rng.random() < 0.6:
                c = rng.choice(CONDS[:-1])
                lines += [f"fcsel {t}{rng.randrange(8, 16)}, {t}{rng.randrange(0, 8)}, {t}{rng.randrange(0, 8)}, {c}"]
            else:
                lines += [select(rng.choice("wx"), None)]
    else:
        w = rng.choice("wx")
        set_lines, d = setter(w, a, b)
        lines += set_lines
        for _ in range(rng.randint(1, 3)):
            lines += [select(w if rng.random() < 0.8 else rng.choice("wx"), d)]
            # (sometimes something between that leaves the flags)
            if rng.random() < 0.2:
                lines += [f"add x{rng.choice(FREE)}, x{rng.choice(FREE)}, #{rng.randrange(1, 4096)}"]
    x = rng.random()
    if x < 0.4:
        target, end = label(), label()
        lines += [f"b.{rng.choice(CONDS[:-1])} {target}", f"add x{rng.choice(FREE)}, x{rng.choice(FREE)}, #1",
                  f"b {end}", f"{target}:", f"add x{rng.choice(FREE)}, x{rng.choice(FREE)}, #2", f"{end}:"]
    elif x < 0.5:
        lines += [f"str x{rng.choice(FREE)}, [x22, #{8 * rng.randrange(0, 64)}]"]
    return lines


lines = [".text", ".globl _start", "_start:"]
for x in FREE:
    lines += setreg(f"x{x}", rng.getrandbits(64))
lines += setreg("x22", DATA)
for i in range(8):
    bits = struct.unpack("<Q", struct.pack("<d", rng.choice(FLOATS)))[0]
    lines += setreg("x23", bits) + [f"fmov d{i}, x23"]
    if rng.random() < 0.5:
        sbits = struct.unpack("<I", struct.pack("<f", rng.choice(FLOATS[:-1])))[0]
        lines += setreg("x23", sbits) + [f"fmov s{i}, w23"]
for _ in range(rng.randint(20, 60)):
    lines += segment()
lines += ["svc #0"]
open(out, "w").write("\n".join(lines) + "\n")
