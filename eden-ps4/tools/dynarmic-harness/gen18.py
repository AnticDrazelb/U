# (04.30) Random A64 programs for bitfield_shifts and the W stores without a zero-extension:
# every SBFM/BFM/UBFM encoding (raw words: all immr/imms, W and X, on edge values), and 32-bit
# results (ALU, shifts, selects, multiplies, loads of every width, W moves of X values) read back
# as 64 bits at once (LSR #32 shows the upper half; 64-bit adds, compares, stores), many live at
# once in one block so some are spilled, some in loops so blocks link.
#   python3 gen18.py <seed> <out.s>
import random
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
DATA = 0x10000000 + 0x1000 * 64
EDGES = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000,
         0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFF7FFFFFFF, 0xFFFFFFFF80000000,
         0x00000000FFFFFF80, 0x8000000080000000, 0x5555555555555555, 0xAAAAAAAAAAAAAAAA]
FREE = list(range(0, 18))
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
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
    return rng.choice(EDGES) if rng.random() < 0.5 else rng.getrandbits(64)


def r():
    return rng.choice(FREE)


def bitfield():
    sf = rng.randrange(2)
    opc = rng.choice([0, 0, 1, 2, 2])  # SBFM, BFM, UBFM
    top = 64 if sf else 32
    k = rng.random()
    if k < 0.3:
        # the aliases' shapes: LSL/UBFIZ/SBFIZ (imms < immr), LSR/ASR (imms = top - 1), extensions (immr 0)
        immr = rng.randrange(1, top)
        imms = rng.choice([immr - 1, rng.randrange(0, immr), top - 1, 7, 15, 31 if top == 64 else 7])
        imms = min(imms, top - 1)
    elif k < 0.4:
        immr, imms = 0, rng.choice([7, 15, 31] if top == 64 else [7, 15])
    else:
        immr, imms = rng.randrange(top), rng.randrange(top)
    word = (sf << 31) | (opc << 29) | (0b100110 << 23) | (sf << 22) | (immr << 16) | (imms << 10) | (r() << 5) | r()
    return [f".inst 0x{word:08x}"]


def w_result(d):
    a, b, c = r(), r(), r()
    pre = []
    if rng.random() < 0.4:
        # an operand made by a 64-bit operation in the same block (its W is the low half of a host
        # register holding all 64 bits)
        pre = [f"{rng.choice(['add', 'eor', 'sub', 'orr'])} x{a}, x{r()}, x{r()}"]
        if rng.random() < 0.5:
            return pre + [f"{rng.choice(['mov', 'uxtw'])} {'w' if rng.random() < 0.5 else 'x'}{d}, w{a}".replace("uxtw w", "uxtw x").replace("mov x", "mov w")]
    return pre + w_result2(d, a, b, c)


def w_result2(d, a, b, c):
    k = rng.randrange(22)
    if k == 0: return [f"add w{d}, w{a}, w{b}"]
    if k == 1: return [f"add w{d}, w{a}, #{rng.randrange(4096)}"]
    if k == 2: return [f"sub w{d}, w{a}, w{b}"]
    if k == 3: return [f"sub w{d}, w{a}, #{rng.randrange(4096)}"]
    if k == 4: return [f"mul w{d}, w{a}, w{b}"]
    if k == 5: return [f"and w{d}, w{a}, w{b}"]
    if k == 6: return [f"orr w{d}, w{a}, w{b}"]
    if k == 7: return [f"eor w{d}, w{a}, w{b}"]
    if k == 8: return [f"mvn w{d}, w{a}"]
    if k == 9: return [f"neg w{d}, w{a}"]
    if k == 10: return [f"csel w{d}, w{a}, w{b}, {rng.choice(CONDS)}"]
    if k == 11: return [f"{rng.choice(['lsl', 'lsr', 'asr'])} w{d}, w{a}, #{rng.randrange(32)}"]
    if k == 12: return [f"{rng.choice(['lslv', 'lsrv', 'asrv', 'rorv'])} w{d}, w{a}, w{b}"]
    if k == 13: return [f"mov w{d}, w{a}"]
    if k == 14: return [f"ldr w{d}, [x22, #{4 * rng.randrange(128)}]"]
    if k == 15: return [f"ldrb w{d}, [x22, #{rng.randrange(512)}]"]
    if k == 16: return [f"ldrh w{d}, [x22, #{2 * rng.randrange(256)}]"]
    if k == 17: return [f"{rng.choice(['ldrsb', 'ldrsh'])} w{d}, [x22, #{2 * rng.randrange(256)}]"]
    if k == 18: return [f"madd w{d}, w{a}, w{b}, w{c}"]
    if k == 19: return [f"{rng.choice(['add', 'sub', 'and', 'orr', 'eor'])} w{d}, w{a}, w{b}, {rng.choice(['lsl', 'lsr', 'asr'])} #{rng.randrange(32)}"]
    if k == 20: return [f"{rng.choice(['uxtb', 'uxth', 'sxtb', 'sxth'])} w{d}, w{a}"]
    return [f"{rng.choice(['csinc', 'csinv', 'csneg'])} w{d}, w{a}, w{b}, {rng.choice(CONDS)}"]


def x_use(d):
    e, f = r(), r()
    k = rng.randrange(6)
    if k == 0: return [f"lsr x{e}, x{d}, #32"]
    if k == 1: return [f"add x{e}, x{d}, x{f}"]
    if k == 2: return [f"str x{d}, [x22, #{8 * rng.randrange(64)}]"]
    if k == 3: return [f"cmp x{d}, x{f}", f"cset x{e}, {rng.choice(CONDS)}"]
    if k == 4: return [f"eor x{e}, x{f}, x{d}, ror #{rng.randrange(64)}"]
    return [f"add x{e}, x{f}, x{d}, lsl #{rng.randrange(1, 5)}"]


def segment():
    lines = []
    k = rng.random()
    if k < 0.08:
        lines += setreg(f"x{r()}", value())
    if k < 0.4:
        for _ in range(rng.randint(1, 4)):
            lines += bitfield()
    elif k < 0.75:
        d = r()
        lines += w_result(d)
        for _ in range(rng.randint(1, 3)):
            lines += x_use(d)
    elif k < 0.85:
        # many 32-bit results live at once (spills), then each read as 64 bits
        ds = rng.sample(FREE, 12)
        for d in ds:
            lines += w_result(d)
        for d in ds:
            lines += x_use(d)
    elif k < 0.93:
        # a W result reaching another block: a branch between it and its 64-bit use
        d = r()
        lbl = label()
        lines += w_result(d) + [f"cbnz x{r()}, {lbl}"] + [f"add x{r()}, x{r()}, #1"] + [f"{lbl}:"] + x_use(d)
    else:
        # a store of the low bits of an X value (STR W, STRB, STRH) then a load back
        off = 8 * rng.randrange(64)
        lines += [f"{rng.choice(['str w', 'strb w', 'strh w'])}{r()}, [x22, #{off}]", f"ldr x{r()}, [x22, #{off}]"]
    return lines


lines = [".text", ".globl _start", "_start:"]
for x in FREE:
    lines += setreg(f"x{x}", value())
lines += setreg("x22", DATA)
# data for the loads
for i in range(0, 512, 8):
    lines += setreg("x23", value()) + [f"str x23, [x22, #{i}]"]
for _ in range(rng.randint(30, 70)):
    lines += segment()
body = []
for _ in range(rng.randint(5, 15)):
    body += segment()
lines += ["mov x19, #5", "LOOP1:"] + body + ["subs x19, x19, #1", "b.ne LOOP1"]
lines += ["svc #0"]
open(out, "w").write("\n".join(lines) + "\n")
