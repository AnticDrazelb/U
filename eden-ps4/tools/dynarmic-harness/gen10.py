# Random ARM programs of flags across blocks: flag setters (all four flags, or only N and Z, or N, Z
# and the shifter's C) before conditional and unconditional branches, loops and calls, and at the
# code those lead to, every kind of instruction that reads flags: conditional instructions,
# ADC/SBC/RSC, RRX, conditional branches on each flag, MRS (all of them, into memory), functions that
# read their caller's flags and functions that hand flags back. For the differential tests of the
# dead flags at a block's exit (04.02: a block reads the code its successors start with).
#   python3 gen10.py <seed> arm <out.s>
import random
import sys

seed, mode, out = int(sys.argv[1]), sys.argv[2], sys.argv[3]
rng = random.Random(seed)
DATA = 0x10000000 + 0x1000 * 64
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
FREE = list(range(0, 10))  # r10, r11: loop counters, r12: data base
label_n = 0
slot_n = 0


def label():
    global label_n
    label_n += 1
    return f"L{label_n}"


def reg():
    return f"r{rng.choice(FREE)}"


def imm():
    return rng.choice([0, 1, 2, 7, 31, 255, 0x100, 0xff00, 0x80000000, 0xc0000003, 0xf000000f])


def setter():
    n, m, d = reg(), reg(), reg()
    k = rng.random()
    if k < 0.3:
        op = rng.choice(["cmp", "cmn", "cmp", "cmp"])
        return [f"{op} {n}, {m}" if rng.random() < 0.5 else f"{op} {n}, #{imm()}"]
    if k < 0.45:
        op = rng.choice(["tst", "teq"])  # N, Z (and C from a rotated immediate or a shift)
        x = rng.random()
        if x < 0.4:
            return [f"{op} {n}, #{imm()}"]
        if x < 0.7:
            return [f"{op} {n}, {m}"]
        return [f"{op} {n}, {m}, {rng.choice(['lsl', 'lsr', 'asr', 'ror'])} #{rng.randrange(0, 32) or 1}"]
    if k < 0.65:
        op = rng.choice(["adds", "subs", "rsbs", "adcs", "sbcs"])
        return [f"{op} {d}, {n}, {m}" if rng.random() < 0.5 else f"{op} {d}, {n}, #{imm()}"]
    if k < 0.8:
        op = rng.choice(["movs", "mvns", "ands", "orrs", "eors", "bics"])
        if op in ("movs", "mvns"):
            x = rng.random()
            if x < 0.3:
                return [f"{op} {d}, {m}"]  # LSL #0: C kept
            if x < 0.6:
                return [f"{op} {d}, {m}, {rng.choice(['lsl', 'lsr', 'asr', 'ror'])} #{rng.randrange(1, 32)}"]
            if x < 0.8:
                return [f"{op} {d}, {m}, rrx"]
            return [f"{op} {d}, #{imm()}"]
        return [f"{op} {d}, {n}, {m}" if rng.random() < 0.5 else f"{op} {d}, {n}, #{imm()}"]
    if k < 0.9:
        return [f"{rng.choice(['lsls', 'lsrs', 'asrs'])} {d}, {n}, {m}"]  # by a register: by 0 keeps C
    if k < 0.95:
        return [f"muls {d}, {n}, {m}"]
    return [f"vcmp.f32 s{rng.randrange(0, 16)}, s{rng.randrange(0, 16)}", "vmrs APSR_nzcv, fpscr"]


def save_flags():
    global slot_n
    slot_n += 1
    r = reg()
    return [f"mrs {r}, apsr", f"str {r}, [r12, #{4 * (slot_n % 60)}]"]


def reader():
    d, n, m = reg(), reg(), reg()
    c = rng.choice(CONDS)
    k = rng.random()
    if k < 0.25:
        return [f"{rng.choice(['add', 'sub', 'mov', 'eor'])}{c} {d}, {n}, #{rng.randrange(1, 256)}".replace(f"mov{c} {d}, {n}, ", f"mov{c} {d}, ")]
    if k < 0.4:
        return [f"{rng.choice(['adc', 'sbc', 'rsc'])} {d}, {n}, {m}"]
    if k < 0.5:
        return [f"mov {d}, {n}, rrx"]
    if k < 0.65:
        return save_flags()
    if k < 0.75:
        return [f"str{c} {d}, [r12, #{4 * rng.randrange(0, 60)}]"]
    if k < 0.85:
        return [f"cmp{c} {n}, {m}"]  # a conditional setter: reads, and doesn't set for sure
    return [f"vmov{c}.f32 s{rng.randrange(0, 16)}, s{rng.randrange(0, 16)}"]


def plain():
    d, n, m = reg(), reg(), reg()
    k = rng.random()
    if k < 0.4:
        return [f"{rng.choice(['add', 'sub', 'eor', 'orr', 'and', 'bic'])} {d}, {n}, {m}"]
    if k < 0.6:
        return [f"{rng.choice(['ldr', 'str'])} {d}, [r12, #{4 * rng.randrange(0, 60)}]"]
    if k < 0.7:
        return [f"movw {d}, #{rng.randrange(0, 65536)}"]
    if k < 0.8:
        return [f"{rng.choice(['uxtb', 'sxth', 'clz', 'rev'])} {d}, {n}"]
    if k < 0.9:
        return [f"vadd.f32 s{rng.randrange(0, 16)}, s{rng.randrange(0, 16)}, s{rng.randrange(0, 16)}"]
    return [f"mul {d}, {n}, {m}"]


def run(k):
    lines = []
    for _ in range(k):
        x = rng.random()
        lines += setter() if x < 0.3 else reader() if x < 0.3 + READS else plain()
    return lines


# how often a successor reads the flags before it sets them (the rest set them first: the case the
# dead flags are for), by program
READS = rng.choice([0.1, 0.2, 0.35])


def head():
    # what a successor starts with: a few plain instructions, then a reader or a setter (and what
    # follows: more of the program, a branch...)
    lines = []
    for _ in range(rng.randint(0, 3)):
        lines += plain()
    lines += reader() if rng.random() < READS else setter()
    return lines


def segment(depth, funcs):
    x = rng.random()
    lines = []
    if x < 0.3:
        # forward conditional branch: both successors start with heads
        target = label()
        lines += setter() + [f"b{rng.choice(CONDS)} {target}"] + head() + run(rng.randint(0, 3)) + [f"{target}:"] + head()
    elif x < 0.45:
        # a chain of conditional branches on the same flags
        a, b = label(), label()
        lines += setter() + [f"b{rng.choice(CONDS)} {a}", f"b{rng.choice(CONDS)} {b}"] + head() + [f"{a}:"] + head() + [f"{b}:"] + head()
    elif x < 0.65 and depth < 2:
        counter = "r10" if depth == 0 else "r11"
        top = label()
        lines += [f"mov {counter}, #{rng.randint(1, 4)}", f"{top}:"] + head()
        for _ in range(rng.randint(1, 2)):
            lines += segment(depth + 1, funcs)
        lines += setter() if rng.random() < 0.3 else []
        lines += [f"subs {counter}, {counter}, #1", f"bne {top}"] + head()
    elif x < 0.8 and funcs:
        lines += setter() if rng.random() < 0.7 else []
        if rng.random() < 0.7:
            lines += [f"bl{rng.choice(['', '', rng.choice(CONDS)])} {rng.choice(funcs)}"]
        else:
            lines += [f"adr lr, {rng.choice(funcs)}", "blx lr"]
        lines += head()
    elif x < 0.9:
        # an unconditional branch over code into a head
        target = label()
        lines += setter() + [f"b {target}"] + run(rng.randint(1, 3)) + [f"{target}:"] + head()
    else:
        lines += run(rng.randint(1, 5))
    return lines


funcs = [f"F{i}" for i in range(rng.randint(1, 3))]
lines = [".syntax unified", ".arm", ".text", ".globl _start", "_start:"]
for i in range(10):
    v = rng.getrandbits(32)
    lines += [f"movw r{i}, #{v & 0xFFFF}", f"movt r{i}, #{v >> 16}"]
lines += [f"movw r12, #{DATA & 0xFFFF}", f"movt r12, #{DATA >> 16}"]
for i in range(16):
    lines += [f"vmov s{i}, r{i % 10}"]
lines += [f"msr APSR_nzcvq, #{rng.choice([0, 0x10000000, 0x20000000, 0x40000000, 0x80000000, 0xf0000000, 0x60000000])}"]
for _ in range(rng.randint(6, 14)):
    lines += segment(0, funcs)
lines += save_flags() + ["svc #0"]
for f in funcs:
    lines += [".p2align 2", f"{f}:"]
    # functions that read the caller's flags first, set their own and hand them back, or neither
    x = rng.random()
    if x < 0.35:
        lines += reader() + run(rng.randint(0, 3))
    elif x < 0.7:
        lines += plain() + setter() + run(rng.randint(0, 2))
    else:
        lines += run(rng.randint(1, 4))
    lines += ["bx lr"]
open(out, "w").write("\n".join(lines) + "\n")
