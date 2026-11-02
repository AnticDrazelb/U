# Random A64 programs of control flow and flags: ALU ops on many live registers (W and X), flag
# setters, CSEL/CSINC/CSET/CCMP, conditional branches forward, CBZ/CBNZ/TBZ, bounded loops,
# calls and returns, loads and stores, FP arithmetic: for the differential tests of the register
# allocator and the block links (dynbench64 test, old build vs new).
#   python3 gen8.py <seed> <out.s>
import random
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
DATA = 0x10000000 + 0x1000 * 64
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
FREE = list(range(0, 18))  # x18 untouched, x19..x21: loop counters, x22: data base
n_label = 0


def label():
    global n_label
    n_label += 1
    return f"L{n_label}"


def r(w=None):
    w = rng.choice("wx") if w is None else w
    return f"{w}{rng.choice(FREE)}"


def setreg(reg, value):
    lines = [f"movz {reg}, #{value & 0xFFFF}"]
    for s in (16, 32, 48):
        lines.append(f"movk {reg}, #{(value >> s) & 0xFFFF}, lsl #{s}")
    return lines


def alu():
    w = rng.choice("wx")
    d, n, m = r(w), r(w), r(w)
    bits = 32 if w == "w" else 64
    k = rng.random()
    if k < 0.35:
        op = rng.choice(["add", "sub", "adds", "subs", "and", "orr", "eor", "bic", "ands", "orn", "eon"])
        if rng.random() < 0.5:
            return [f"{op} {d}, {n}, {m}"]
        sh = rng.choice(["lsl", "lsr", "asr"] + ([] if op.startswith(("add", "sub")) else ["ror"]))
        return [f"{op} {d}, {n}, {m}, {sh} #{rng.randrange(0, bits)}"]
    if k < 0.45:
        op = rng.choice(["add", "sub", "adds", "subs"])
        return [f"{op} {d}, {n}, #{rng.randrange(0, 4096)}"]
    if k < 0.55:
        op = rng.choice(["adc", "sbc", "adcs", "sbcs"])
        return [f"{op} {d}, {n}, {m}"]
    if k < 0.65:
        op = rng.choice(["lsl", "lsr", "asr", "ror"])
        if rng.random() < 0.5:
            return [f"{op} {d}, {n}, #{rng.randrange(0, bits)}"]
        return [f"{op} {d}, {n}, {m}"]
    if k < 0.75:
        op = rng.choice(["mul", "madd", "msub", "udiv", "sdiv", "umulh", "smulh"])
        if op in ("umulh", "smulh"):
            return [f"{op} {r('x')}, {r('x')}, {r('x')}"]
        if op in ("madd", "msub"):
            return [f"{op} {d}, {n}, {m}, {r(w)}"]
        return [f"{op} {d}, {n}, {m}"]
    if k < 0.9:
        c = rng.choice(CONDS)
        op = rng.choice(["csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc", "ccmp", "ccmn"])
        if op in ("cset", "csetm"):
            return [f"{op} {d}, {c}"]
        if op == "cinc":
            return [f"cinc {d}, {n}, {c}"]
        if op in ("ccmp", "ccmn"):
            return [f"{op} {n}, #{rng.randrange(0, 32)}, #{rng.randrange(0, 16)}, {c}"]
        return [f"{op} {d}, {n}, {m}, {c}"]
    op = rng.choice(["ubfx", "sbfx", "rev", "clz", "rbit", "uxtb", "sxth", "extr"])
    if op in ("ubfx", "sbfx"):
        lsb = rng.randrange(0, bits - 1)
        return [f"{op} {d}, {n}, #{lsb}, #{rng.randrange(1, bits - lsb)}"]
    if op == "extr":
        return [f"extr {d}, {n}, {m}, #{rng.randrange(0, bits)}"]
    if op in ("uxtb",):
        return [f"uxtb {r('w')}, {r('w')}"]
    if op == "sxth":
        return [f"sxth {r('x')}, {r('w')}"]
    return [f"{op} {d}, {n}"]


def flags():
    w = rng.choice("wx")
    n, m = r(w), r(w)
    op = rng.choice(["cmp", "cmn", "tst"])
    if rng.random() < 0.5:
        return [f"{op} {n}, {m}"]
    if op == "tst":
        return [f"tst {n}, #{rng.choice([1, 0xff, 0x80, 0xf0])}"]
    return [f"{op} {n}, #{rng.randrange(0, 4096)}"]


def mem():
    t = rng.choice(FREE)
    off = 8 * rng.randrange(0, 64)
    k = rng.random()
    if k < 0.3:
        return [f"ldr x{t}, [x22, #{off}]"]
    if k < 0.55:
        return [f"str x{t}, [x22, #{off}]"]
    if k < 0.7:
        return [f"{rng.choice(['ldrb', 'ldrh', 'ldr'])} w{t}, [x22, #{off // 2 * 2}]"]
    if k < 0.85:
        t2 = rng.choice([x for x in FREE if x != t])
        return [f"{rng.choice(['ldp', 'stp'])} x{t}, x{t2}, [x22, #{off // 16 * 16}]"]
    return [f"{rng.choice(['strb', 'strh', 'str'])} w{t}, [x22, #{off}]"]


def fp():
    d, n, m = (f"s{rng.randrange(0, 16)}" if rng.random() < 0.5 else f"d{rng.randrange(0, 16)}" for _ in range(1)), 0, 0
    kind = rng.choice("sd")
    d, n, m = (f"{kind}{rng.randrange(0, 16)}" for _ in range(3))
    k = rng.random()
    if k < 0.5:
        return [f"{rng.choice(['fadd', 'fsub', 'fmul', 'fmax', 'fmin'])} {d}, {n}, {m}"]
    if k < 0.65:
        return [f"fcmp {n}, {m}"]
    if k < 0.8:
        return [f"fcsel {d}, {n}, {m}, {rng.choice(CONDS)}"]
    return [f"fmov {d}, {'w' if kind == 's' else 'x'}{rng.choice(FREE)}"]


def straight(k):
    lines = []
    for _ in range(k):
        x = rng.random()
        if x < 0.5:
            lines += alu()
        elif x < 0.65:
            lines += flags()
        elif x < 0.85:
            lines += mem()
        else:
            lines += fp()
    return lines


def segment(depth, funcs):
    x = rng.random()
    lines = []
    if x < 0.3:
        lines += straight(rng.randint(1, 8))
    elif x < 0.5:
        target = label()
        lines += flags() if rng.random() < 0.7 else []
        lines += [f"b.{rng.choice(CONDS)} {target}"] + straight(rng.randint(0, 5)) + [f"{target}:"]
    elif x < 0.6:
        target = label()
        reg = r()
        k = rng.random()
        if k < 0.6:
            lines += [f"{rng.choice(['cbz', 'cbnz'])} {reg}, {target}"]
        else:
            lines += [f"{rng.choice(['tbz', 'tbnz'])} {reg}, #{rng.randrange(0, 32 if reg[0] == 'w' else 64)}, {target}"]
        lines += straight(rng.randint(0, 5)) + [f"{target}:"]
    elif x < 0.8 and depth < 3:
        counter = f"x{19 + depth}"
        top = label()
        lines += [f"mov {counter}, #{rng.randint(1, 5)}", f"{top}:"]
        for _ in range(rng.randint(1, 3)):
            lines += segment(depth + 1, funcs)
        if rng.random() < 0.5:
            lines += [f"subs {counter}, {counter}, #1", f"b.ne {top}"]
        else:
            lines += [f"sub {counter}, {counter}, #1", f"cbnz {counter}, {top}"]
    elif x < 0.9 and funcs:
        if rng.random() < 0.5:
            lines += [f"bl {rng.choice(funcs)}"]
        else:
            lines += [f"adr x30, {rng.choice(funcs)}", "blr x30"]  # indirect (fast dispatch)
    else:
        other, end = label(), label()
        lines += flags() + [f"b.{rng.choice(CONDS)} {other}"] + straight(rng.randint(1, 4)) + [f"b {end}", f"{other}:"]
        lines += straight(rng.randint(1, 4)) + [f"{end}:"]
    return lines


funcs = [f"F{i}" for i in range(rng.randint(0, 3))]
lines = [".text", ".globl _start", "_start:"]
for x in FREE:
    lines += setreg(f"x{x}", rng.getrandbits(64))
lines += setreg("x22", DATA)
for v in range(16):
    lines += [f"fmov d{v}, x{v}"]
lines += flags()
for _ in range(rng.randint(6, 20)):
    lines += segment(0, funcs)
lines += ["svc #0"]
for f in funcs:
    lines += [".p2align 2", f"{f}:"]
    for _ in range(rng.randint(1, 3)):
        if rng.random() < 0.6:
            lines += straight(rng.randint(1, 6))
        else:
            target = label()
            lines += flags() + [f"b.{rng.choice(CONDS)} {target}"] + straight(rng.randint(0, 4)) + [f"{target}:"]
    lines += ["ret"]
lines += [""]
open(out, "w").write("\n".join(lines))
