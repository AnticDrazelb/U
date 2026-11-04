# (04.24) Random A64 programs of calls and returns around FPCR changes: functions (BL, BLR) that
# set the rounding mode or flush-to-zero (MSR FPCR) or not before returning, nested calls, and float
# work after each return whose result depends on the mode (FRINTI, FCVTZS of FRINTX, FADD of values
# that round differently, denormals): for return_stack_pc_only (a return stack entry must not take
# a return to code translated for another FPCR).
#   python3 gen14.py <seed> <out.s>
import random
import struct
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
DATA = 0x10000000 + 0x1000 * 64
N_FUNCS = 6
MODES = [0, 1 << 22, 2 << 22, 3 << 22, 1 << 24, (1 << 24) | (2 << 22)]
VALUES = [2.5, -2.5, 1.5, 0.1, 1e-310, -1e-310, 3.0e300, 0.3333333333333333, 7.75, -0.5]


def setreg(reg, value):
    lines = [f"movz {reg}, #{value & 0xFFFF}"]
    for s in (16, 32, 48):
        lines.append(f"movk {reg}, #{(value >> s) & 0xFFFF}, lsl #{s}")
    return lines


def setmode():
    return setreg("x9", rng.choice(MODES)) + ["msr fpcr, x9"]


def setmode_dynamic():
    # (the mode the caller chose this time: one return site reached under several modes, so that a
    # stale return stack entry would have code translated for another one to jump to)
    return ["msr fpcr, x20"]


def floatwork():
    a, b = rng.randrange(0, 8), rng.randrange(0, 8)
    d = rng.randrange(8, 16)
    k = rng.random()
    if k < 0.3:
        body = [f"frinti d{d}, d{a}"]
    elif k < 0.5:
        body = [f"frintx d{d}, d{a}", f"fcvtzs x{rng.randrange(10, 18)}, d{d}"]
    elif k < 0.7:
        body = [f"fadd d{d}, d{a}, d{b}"]
    elif k < 0.85:
        body = [f"fmul d{d}, d{a}, d{b}"]
    else:
        body = [f"fcvt s{d}, d{a}", f"fcvt d{d}, s{d}"]
    return body + [f"str d{d}, [x22, #{8 * rng.randrange(0, 128)}]"]


def call(k):
    if rng.random() < 0.7:
        return [f"bl F{k}"]
    return [f"adr x8, F{k}", "blr x8"]


lines = [".text", ".globl _start", "_start:"]
lines += setreg("x22", DATA)
for i in range(8):
    bits = struct.unpack("<Q", struct.pack("<d", rng.choice(VALUES)))[0]
    lines += setreg("x9", bits) + [f"fmov d{i}, x9"]
lines += ["mov x19, #4", "mov x21, #0", "mrs x20, fpcr", "OUTER:"]
for _ in range(rng.randint(10, 30)):
    if rng.random() < 0.2:
        lines += setmode()
    x = rng.random()
    if x < 0.5:
        # (a mode that changes from one run of this call to the next: x21 counts them)
        lines += ["add x21, x21, #1", "and x20, x21, #3", "lsl x20, x20, #22"]
    elif x < 0.75:
        lines += setreg("x20", rng.choice(MODES))
    else:
        lines += ["mrs x20, fpcr"]
    lines += call(rng.randrange(0, N_FUNCS))
    for _ in range(rng.randint(1, 3)):
        lines += floatwork()
lines += ["subs x19, x19, #1", "b.ne OUTER", "svc #0"]
for k in range(N_FUNCS):
    lines += [".p2align 2", f"F{k}:", "stp x29, x30, [sp, #-16]!"]
    for _ in range(rng.randint(0, 2)):
        lines += floatwork()
    x = rng.random()
    if x < 0.2:
        lines += setmode()
    elif x < 0.8:
        lines += setmode_dynamic()
    if k + 1 < N_FUNCS and rng.random() < 0.4:
        lines += call(rng.randrange(k + 1, N_FUNCS))
        lines += floatwork()
    lines += ["ldp x29, x30, [sp], #16", "ret"]
open(out, "w").write("\n".join(lines) + "\n")
