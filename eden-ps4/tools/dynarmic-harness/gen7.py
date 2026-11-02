# Random A32/Thumb programs of control flow and flags: ALU ops on many live registers, flag
# setters, conditional instructions (IT blocks in Thumb), conditional branches forward, bounded
# loops (backward branches), calls and returns, loads and stores, VFP arithmetic. For the
# differential tests of the block structure (B<c> terminals), the register allocator and the links.
#   python3 gen7.py <seed> <arm|thumb> <out.s>
import random
import sys

seed, mode, out = int(sys.argv[1]), sys.argv[2], sys.argv[3]
rng = random.Random(seed)
thumb = mode == "thumb"
DATA = 0x10000000 + 0x1000 * 64
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
FREE = list(range(0, 10))  # r10, r11: loop counters, r12: data base
label_n = 0


def label():
    global label_n
    label_n += 1
    return f"L{label_n}"


def reg():
    return f"r{rng.choice(FREE)}"


def setreg(r, value):
    return [f"movw {r}, #{value & 0xFFFF}", f"movt {r}, #{(value >> 16) & 0xFFFF}"]


def alu(cond=""):
    d, n, m = reg(), reg(), reg()
    s = rng.choice(["", "s"]) if not cond or not thumb else ""
    k = rng.random()
    if k < 0.45:
        op = rng.choice(["add", "sub", "eor", "orr", "and", "bic", "adc", "sbc", "rsb"])
        if op == "rsb" and thumb:
            return [f"rsb{s}{cond} {d}, {n}, #{rng.randrange(0, 256)}"]
        if rng.random() < 0.5:
            return [f"{op}{s}{cond} {d}, {n}, {m}"]
        if rng.random() < 0.5:
            sh = rng.choice(["lsl", "lsr", "asr", "ror"])
            return [f"{op}{s}{cond} {d}, {n}, {m}, {sh} #{rng.randrange(1, 31)}"]
        return [f"{op}{s}{cond} {d}, {n}, #{rng.choice([1, 4, 255, 0x100, 0xff00, 0x80000000])}"]
    if k < 0.6:
        op = rng.choice(["mov", "mvn"])
        if rng.random() < 0.5:
            return [f"{op}{s}{cond} {d}, #{rng.randrange(0, 256)}"]
        return [f"{op}{s}{cond} {d}, {m}"]
    if k < 0.75:
        sh = rng.choice(["lsl", "lsr", "asr", "ror"])
        if rng.random() < 0.5:
            return [f"{sh}{s}{cond} {d}, {n}, #{rng.randrange(1, 31)}"]
        return [f"{sh}{s}{cond} {d}, {n}, {m}"]
    if k < 0.85:
        d2 = reg()
        while d2 == d:
            d2 = reg()
        op = rng.choice(["mul", "mla", "umull", "smull"])
        if op == "mul":
            return [f"mul{cond} {d}, {n}, {m}"]
        if op == "mla":
            return [f"mla{cond} {d}, {n}, {m}, {reg()}"]
        return [f"{op}{cond} {d}, {d2}, {n}, {m}"]
    if k < 0.93:
        return [f"{rng.choice(['ubfx', 'sbfx'])}{cond} {d}, {n}, #{rng.randrange(0, 16)}, #{rng.randrange(1, 16)}"]
    return [f"{rng.choice(['uxtb', 'sxtb', 'uxth', 'sxth', 'rev', 'clz'])}{cond} {d}, {n}"]


def flags():
    n, m = reg(), reg()
    op = rng.choice(["cmp", "cmn", "tst", "teq"])
    if rng.random() < 0.5:
        return [f"{op} {n}, {m}"]
    return [f"{op} {n}, #{rng.choice([0, 1, 7, 255, 0x80000000])}"]


def vfp_cond(c):
    d, n, m = (f"s{rng.randrange(0, 16)}" for _ in range(3))
    k = rng.random()
    if k < 0.3:
        return [f"vmov{c}.f32 {d}, {n}"]
    if k < 0.5:
        return [f"vmov{c} {d}, {reg()}"]
    if k < 0.7:
        return [f"vmov{c} {reg()}, {n}"]
    if k < 0.85:
        return [f"vadd{c}.f32 {d}, {n}, {m}"]
    dd, dn = rng.randrange(0, 8), rng.randrange(0, 8)
    return [f"vmov{c}.f64 d{dd}, d{dn}"]


def cmp_cond(c):
    n, m = reg(), reg()
    op = rng.choice(["cmp", "cmn", "tst", "teq", "cmp", "cmn"])
    if rng.random() < 0.5:
        return [f"{op}{c} {n}, {m}"]
    return [f"{op}{c} {n}, #{rng.choice([0, 1, 7, 255, 0x80000000])}"]


def cond_inst():
    c = rng.choice(CONDS)
    body = rng.choice([alu, alu, mem, cmp_cond] + ([] if thumb else [vfp_cond]))(c)
    body = body[-1:]  # one instruction
    if thumb:
        return [f"it {c}"] + body
    return body


def mem(cond=""):
    r = reg()
    off = 4 * rng.randrange(0, 64)
    op = rng.choice(["ldr", "str", "ldrb", "strb", "ldrh", "strh"])
    if op.endswith("h"):
        off &= ~1
    return [f"{op}{cond} {r}, [r12, #{off}]"]


def vfp():
    d, n, m = (f"s{rng.randrange(0, 16)}" for _ in range(3))
    op = rng.random()
    if op < 0.6:
        return [f"{rng.choice(['vadd', 'vsub', 'vmul'])}.f32 {d}, {n}, {m}"]
    if op < 0.8:
        return [f"vmov {d}, {reg()}"]
    return [f"vmov {reg()}, {n}"]


def fcmp():
    n, m = rng.randrange(0, 16), rng.randrange(0, 16)
    op = rng.choice(["vcmp", "vcmpe"])
    k = rng.random()
    if k < 0.6:
        cmp = f"{op}.f32 s{n}, s{m}"
    elif k < 0.8:
        cmp = f"{op}.f32 s{n}, #0"
    else:
        cmp = f"{op}.f64 d{n % 8}, d{m % 8}"
    lines = [cmp]
    if rng.random() < 0.3:
        lines += [f"vadd.f32 s{rng.randrange(0, 16)}, s{n}, s{m}"]  # between the compare and the move
    lines += ["vmrs APSR_nzcv, fpscr"]
    return lines


def straight(k):
    lines = []
    for _ in range(k):
        x = rng.random()
        if x < 0.06:
            lines += fcmp()
        elif x < 0.45:
            lines += alu()
        elif x < 0.6:
            lines += flags()
        elif x < 0.75:
            lines += cond_inst()
        elif x < 0.88:
            lines += mem()
        else:
            lines += vfp()
    return lines


def segment(depth, funcs):
    x = rng.random()
    lines = []
    if x < 0.35:
        lines += straight(rng.randint(1, 8))
    elif x < 0.6:
        # forward conditional branch over a few instructions
        target = label()
        x2 = rng.random()
        lines += flags() if x2 < 0.55 else fcmp() if x2 < 0.8 else []
        lines += [f"b{rng.choice(CONDS)} {target}"]
        lines += straight(rng.randint(0, 5))
        lines += [f"{target}:"]
    elif x < 0.8 and depth < 2:
        counter = "r10" if depth == 0 else "r11"
        top = label()
        lines += [f"mov {counter}, #{rng.randint(1, 6)}", f"{top}:"]
        for _ in range(rng.randint(1, 3)):
            lines += segment(depth + 1, funcs)
        lines += [f"subs {counter}, {counter}, #1", f"bne {top}"]
    elif x < 0.9 and funcs:
        if rng.random() < 0.5:
            lines += [f"bl {rng.choice(funcs)}"]
        else:
            # an indirect call (fast dispatch): through lr, or a register saved around it
            lines += [f"adr lr, {rng.choice(funcs)}"] + (["orr lr, lr, #1"] if thumb else []) + ["blx lr"]
    else:
        # conditional branch both ways: if/else
        other, end = label(), label()
        lines += flags() + [f"b{rng.choice(CONDS)} {other}"] + straight(rng.randint(1, 4)) + [f"b {end}", f"{other}:"]
        lines += straight(rng.randint(1, 4)) + [f"{end}:"]
    return lines


funcs = [f"F{i}" for i in range(rng.randint(0, 3))]
lines = [".syntax unified", ".thumb" if thumb else ".arm", ".text", ".globl _start", "_start:"]
for r in range(10):
    lines += setreg(f"r{r}", rng.getrandbits(32))
lines += setreg("r12", DATA)
for s in range(16):
    lines += [f"vmov s{s}, r{s % 10}"]
lines += flags()
for _ in range(rng.randint(6, 20)):
    lines += segment(0, funcs)
lines += ["svc #0"]
for f in funcs:
    lines += [".p2align 2", f"{f}:"]
    for _ in range(rng.randint(1, 3)):
        x = rng.random()
        if x < 0.6:
            lines += straight(rng.randint(1, 6))
        else:
            target = label()
            lines += flags() + [f"b{rng.choice(CONDS)} {target}"] + straight(rng.randint(0, 4)) + [f"{target}:"]
    lines += ["bx lr"]
lines += [""]
open(out, "w").write("\n".join(lines))
