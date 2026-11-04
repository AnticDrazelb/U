# (04.23) Random A64 programs of conditional branches right after what sets their condition: every
# flag setter (CMP/CMN/SUBS/ADDS/TST/ANDS, W and X, register, shifted register, immediate) followed at
# once by every B.cond, and CBZ/CBNZ/TBZ/TBNZ on every bit, on edge values (0, -1, the sign bits,
# the 32-bit boundaries): for FuseCheckBit and FuseCondFlags, which branch on the host's flags.
#   python3 gen11.py <seed> <out.s>
import random
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
DATA = 0x10000000 + 0x1000 * 64
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
EDGES = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000,
         0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFF7FFFFFFF, 0xFFFFFFFF80000000, 4095, 4096]
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


def setter(w, a, b):
    n, m = f"{w}{a}", f"{w}{b}"
    bits = 32 if w == "w" else 64
    k = rng.random()
    d = f"{w}{rng.choice(FREE)}"
    if k < 0.25:
        return [f"{rng.choice(['cmp', 'cmn', 'tst'])} {n}, {m}"]
    if k < 0.45:
        return [f"{rng.choice(['subs', 'adds', 'ands'])} {d}, {n}, {m}"]
    if k < 0.6:
        op = rng.choice(["cmp", "cmn", "subs", "adds"])
        imm = rng.choice([0, 1, 4095, rng.randrange(0, 4096)])
        return [f"{op} {n}, #{imm}"] if op in ("cmp", "cmn") else [f"{op} {d}, {n}, #{imm}"]
    if k < 0.75:
        op = rng.choice(["cmp", "cmn", "subs", "adds"])
        sh = f"{rng.choice(['lsl', 'lsr', 'asr'])} #{rng.randrange(0, bits)}"
        return [f"{op} {n}, {m}, {sh}"] if op in ("cmp", "cmn") else [f"{op} {d}, {n}, {m}, {sh}"]
    if k < 0.85:
        return [f"{rng.choice(['adcs', 'sbcs'])} {d}, {n}, {m}"]
    return [f"tst {n}, #{rng.choice([1, 0xff, 0x80, 0xf0, 0x80000000 if w == 'w' else 0x8000000000000000])}"]


def mark(k):
    # what each path does, so the outcome shows in the final registers
    return [f"add x{rng.choice(FREE)}, x{rng.choice(FREE)}, #{k}"]


def segment():
    lines = []
    a, b = rng.choice(FREE), rng.choice(FREE)
    if rng.random() < 0.7:
        lines += setreg(f"x{a}", value())
        lines += setreg(f"x{b}", value())
    x = rng.random()
    target, end = label(), label()
    if x < 0.55:
        w = rng.choice("wx")
        lines += setter(w, a, b)
        lines += [f"b.{rng.choice(CONDS)} {target}"]
    elif x < 0.8:
        reg = f"{rng.choice('wx')}{a}"
        lines += [f"{rng.choice(['cbz', 'cbnz'])} {reg}, {target}"]
    else:
        w = rng.choice("wx")
        reg = f"{w}{a}"
        lines += [f"{rng.choice(['tbz', 'tbnz'])} {reg}, #{rng.randrange(0, 32 if w == 'w' else 64)}, {target}"]
    lines += mark(rng.randrange(1, 4096)) + [f"b {end}", f"{target}:"] + mark(rng.randrange(1, 4096)) + [f"{end}:"]
    # a flags reader after the branch (the stored NZCV still has to be right)
    if rng.random() < 0.4:
        lines += [f"csinc x{rng.choice(FREE)}, x{rng.choice(FREE)}, x{rng.choice(FREE)}, {rng.choice(CONDS)}"]
    return lines


lines = [".text", ".globl _start", "_start:"]
for x in FREE:
    lines += setreg(f"x{x}", rng.getrandbits(64))
lines += setreg("x22", DATA)
for _ in range(rng.randint(20, 60)):
    lines += segment()
# a loop counted down with SUBS / B.NE and one with ADDS / B.CC
lines += ["mov x19, #7", "LOOP1:"] + mark(3) + ["subs x19, x19, #1", "b.ne LOOP1"]
lines += setreg("x20", 0xFFFFFFFFFFFFFFF0) + ["LOOP2:"] + mark(5) + ["adds x20, x20, #1", "b.cc LOOP2"]
lines += ["svc #0"]
open(out, "w").write("\n".join(lines) + "\n")
