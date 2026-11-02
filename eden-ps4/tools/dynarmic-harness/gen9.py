# Random A32/Thumb programs of NEON loads and stores: VLD1/VST1 of 1-4 registers at every element
# size, alignment hint and start register (even, odd), with writeback by the size or a register,
# at element-aligned, unaligned and page-crossing addresses; VLD1 to one lane and to all lanes,
# VLD2-4/VST2-4, VLDM/VSTM, VLDR/VSTR; mixed with NEON and VFP arithmetic on the same registers
# (D and Q views), loops. For the differential test of the whole-register VLD1/VST1 translation.
#   python3 gen9.py <seed> <arm|thumb> <out.s>
import random
import sys

seed, mode, out = int(sys.argv[1]), sys.argv[2], sys.argv[3]
rng = random.Random(seed)
thumb = mode == "thumb"
DATA = 0x10000000 + 0x1000 * 64
BASES = ["r8", "r9", "r10"]  # address registers, set from r12 (the data area) before each access
n_label = 0


def label():
    global n_label
    n_label += 1
    return f"L{n_label}"


def setreg(r, value):
    return [f"movw {r}, #{value & 0xFFFF}", f"movt {r}, #{(value >> 16) & 0xFFFF}"]


def base(ebytes, regs):
    """An address register near a page boundary or anywhere in a few pages, element aligned or not."""
    b = rng.choice(BASES)
    page = rng.randrange(0, 6) * 0x1000
    k = rng.random()
    if k < 0.35:
        off = page + 0x1000 - rng.randrange(1, 8 * regs + 1)  # crosses into the next page
    elif k < 0.7:
        off = page + rng.randrange(0, 0x1000 - 32)
    else:
        off = page + 16 * rng.randrange(0, 250)  # 16-byte aligned
    if rng.random() < 0.7:
        off -= off % ebytes  # element aligned (the architecture's minimum without an alignment hint)
    return b, [f"add {b}, r12, #{off & 0xff00}"] + ([f"add {b}, {b}, #{off & 0xff}"] if off & 0xff else [])


def dlist(regs, start=None, step=1):
    start = rng.randrange(0, 32 - (regs - 1) * step) if start is None else start
    return start, "{" + ", ".join(f"d{start + i * step}" for i in range(regs)) + "}"


def vld1_vst1():
    op = rng.choice(["vld1", "vst1"])
    size = rng.choice([8, 16, 32, 64])
    regs = rng.randint(1, 4)
    start = rng.randrange(0, 33 - regs)
    if rng.random() < 0.5:
        start -= start % 2  # a Q register pair, often
        start = min(start, 32 - regs)
    _, lst = dlist(regs, start)
    b, lines = base(size // 8, regs)
    # an alignment hint only when the address is that aligned (else the access would fault)
    hint = ""
    if rng.random() < 0.3:
        lines.append(f"bic {b}, {b}, #63")
        hint = rng.choice([":64", ":128"] if regs in (2, 4) else [":64"])
        if regs == 4:
            hint = rng.choice([":64", ":128", ":256"])
    wb = rng.random()
    if wb < 0.4:
        addr = f"[{b}{hint}]"
    elif wb < 0.7:
        addr = f"[{b}{hint}]!"
    else:
        addr = f"[{b}{hint}], r11"
    return lines + [f"{op}.{size} {lst}, {addr}"]


def vld_lane():
    size = rng.choice([8, 16, 32])
    b, lines = base(size // 8, 1)
    d = rng.randrange(0, 32)
    if rng.random() < 0.5:
        lane = rng.randrange(0, 64 // size)
        return lines + [f"{rng.choice(['vld1', 'vst1'])}.{size} {{d{d}[{lane}]}}, [{b}]"]
    regs = rng.choice([1, 2])
    d = min(d, 32 - regs)
    return lines + [f"vld1.{size} {{" + ", ".join(f"d{d + i}[]" for i in range(regs)) + f"}}, [{b}]"]


def vld_interleaved():
    nelem = rng.choice([2, 3, 4])
    size = rng.choice([8, 16, 32])
    step = rng.choice([1, 2]) if nelem in (2, 3, 4) else 1
    start, lst = dlist(nelem, None, step)
    b, lines = base(size // 8, nelem)
    return lines + [f"{rng.choice(['vld', 'vst'])}{nelem}.{size} {lst}, [{b}]"]


def vldm():
    regs = rng.randint(1, 8)
    start = rng.randrange(0, 33 - regs)
    b, lines = base(4, regs)
    lines.append(f"bic {b}, {b}, #3")
    lst = "{" + ", ".join(f"d{start + i}" for i in range(regs)) + "}"
    return lines + [f"{rng.choice(['vldmia', 'vstmia'])} {b}, {lst}"]


def vldr():
    b, lines = base(4, 1)
    lines.append(f"bic {b}, {b}, #3")
    if rng.random() < 0.5:
        return lines + [f"{rng.choice(['vldr', 'vstr'])} d{rng.randrange(0, 32)}, [{b}, #{4 * rng.randrange(0, 64)}]"]
    return lines + [f"{rng.choice(['vldr', 'vstr'])} s{rng.randrange(0, 32)}, [{b}, #{4 * rng.randrange(0, 64)}]"]


def arith():
    k = rng.random()
    q = lambda: f"q{rng.randrange(0, 16)}"
    d = lambda: f"d{rng.randrange(0, 32)}"
    if k < 0.25:
        op = rng.choice(["vadd.i32", "vsub.i16", "veor", "vand", "vorr", "vadd.i8", "vmul.i32"])
        r = rng.choice([q, d])
        return [f"{op} {r()}, {r()}, {r()}"]
    if k < 0.45:
        op = rng.choice(["vadd.f32", "vmul.f32", "vmla.f32", "vmls.f32", "vsub.f32"])
        r = rng.choice([q, d])
        return [f"{op} {r()}, {r()}, {r()}"]
    if k < 0.6:
        return [f"vmov {q()}, {q()}"]
    if k < 0.75:
        return [f"vmov {d()}, r{rng.randrange(0, 8)}, r{rng.randrange(0, 8)}"]
    if k < 0.9:
        a = rng.randrange(0, 7)
        return [f"vmov r{a}, r{(a + rng.randrange(1, 7)) % 7}, {d()}"]  # (not r7, the loop counter)
    s = lambda: f"s{rng.randrange(0, 32)}"
    return [f"{rng.choice(['vadd.f32', 'vmul.f32'])} {s()}, {s()}, {s()}"]


def straight(k):
    lines = []
    for _ in range(k):
        x = rng.random()
        if x < 0.35:
            lines += vld1_vst1()
        elif x < 0.45:
            lines += vld_lane()
        elif x < 0.52:
            lines += vld_interleaved()
        elif x < 0.6:
            lines += vldm()
        elif x < 0.66:
            lines += vldr()
        else:
            lines += arith()
    return lines


lines = [".syntax unified", ".thumb" if thumb else ".arm", ".fpu neon", ".text", ".globl _start", "_start:"]
for r in range(8):
    lines += setreg(f"r{r}", rng.getrandbits(32))
lines += setreg("r12", DATA)
lines += setreg("r11", rng.choice([8, 16, 24, 32, 7, 64, 0xFFFFFFF0]))  # the register writeback's step
for i in range(0, 32, 2):
    lines += [f"vmov d{i}, r{i % 8}, r{(i + 3) % 8}", f"vmov d{i + 1}, r{(i + 5) % 8}, r{(i + 1) % 8}"]
# fill some of the data with values the registers then load
for i in range(6):
    lines += setreg("r8", DATA + 0x1000 * i + 0x7F8) + [f"vstmia r8, {{d{2 * i}-d{2 * i + 3}}}"]
for _ in range(rng.randint(3, 8)):
    if rng.random() < 0.3:
        top = label()
        lines += ["mov r7, #3", f"{top}:"] + straight(rng.randint(2, 10)) + ["subs r7, r7, #1", f"bne {top}"]
        lines += setreg("r7", rng.getrandbits(32))
    else:
        lines += straight(rng.randint(2, 12))
lines += ["svc #0", ""]
open(out, "w").write("\n".join(lines))
