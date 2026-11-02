# Random A64 programs of loads and stores sharing base registers (every size, LDUR/STUR, signed
# loads, LDP/STP of W, X, D and Q with offsets, pre- and post-index, register offsets, SIMD
# loads and stores, a prologue and epilogue on SP, a load into its own base, a store of the base
# itself), near page boundaries, on the marked page, next to and on the unmapped one, and outside
# the page table's address space: dynbench64 test ... vs ... nowalk.
#   python3 gen6.py <seed> <out.s>
import random
import sys

seed, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(seed)
DATA, PAGES = 0x10000000, 4096
MARKED, UNMAPPED = 0x10800000, 0x10900000


def addr(align=16):
    kind = rng.random()
    if kind < 0.12:
        page = MARKED
    elif kind < 0.18:
        page = UNMAPPED
    elif kind < 0.26:
        page = UNMAPPED + rng.choice([-4096, 4096])
    elif kind < 0.29:
        page = 0x100000000 + 4096 * rng.randrange(0, 16)  # outside the 32-bit page table
    else:
        page = DATA + 4096 * rng.randrange(16, PAGES - 16)
    off = rng.choice([rng.randrange(0, 128), rng.randrange(4096 - 300, 4096 + 64), rng.randrange(0, 4096)])
    return (page + off) & ~(align - 1)


def setreg(r, value):
    out = [f"movz x{r}, #{value & 0xFFFF}"]
    for sh in (16, 32):
        if (value >> sh) & 0xFFFF:
            out.append(f"movk x{r}, #{(value >> sh) & 0xFFFF}, lsl #{sh}")
    return out


def regs(exclude, n):
    return rng.sample([r for r in range(0, 29) if r not in exclude and r != 18], n)


lines = [".text", ".globl _start", "_start:"]
for _ in range(rng.randint(6, 18)):
    kind = rng.random()
    if kind < 0.55:  # a run of accesses through one base
        base = regs(set(), 1)[0]
        lines += setreg(base, addr())
        for _ in range(rng.randint(2, 8)):
            k = rng.random()
            if k < 0.30:
                size, r = rng.choice([("x", 8), ("w", 4)])
                t = regs({base}, 1)[0]
                op = rng.choice(["ldr", "str"])
                lines.append(f"{op} {size}{t}, [x{base}, #{r * rng.randrange(0, 32)}]")
            elif k < 0.42:
                t = regs({base}, 1)[0]
                op = rng.choice(["ldur", "stur", "ldurb", "sturb", "ldurh", "sturh", "ldursw", "ldursb", "ldursh"])
                reg = "x" if op in ("ldur", "stur", "ldursw") and rng.random() < 0.5 or op == "ldursw" else "w"
                if op in ("ldursb", "ldursh"):
                    reg = rng.choice(["w", "x"])
                lines.append(f"{op} {reg}{t}, [x{base}, #{rng.randrange(-64, 200)}]")
            elif k < 0.52:
                t = regs({base}, 1)[0]
                op = rng.choice(["ldrb w", "strb w", "ldrh w", "strh w", "ldrsb x", "ldrsh w", "ldrsw x"])
                scale = 2 if "h" in op.split()[0][-1:] else (4 if "sw" in op else 1)
                lines.append(f"{op}{t}, [x{base}, #{scale * rng.randrange(0, 64)}]")
            elif k < 0.70:
                size, r = rng.choice([("x", 8), ("w", 4), ("d", 8), ("q", 16)])
                if size in ("x", "w"):
                    t1, t2 = regs({base}, 2)
                else:
                    t1, t2 = rng.sample(range(32), 2)
                op = rng.choice(["ldp", "stp"])
                if op == "ldp" and size in ("x", "w") and t1 == t2:
                    continue
                form = rng.random()
                imm = r * rng.randrange(-4, 12)
                if form < 0.7:
                    lines.append(f"{op} {size}{t1}, {size}{t2}, [x{base}, #{imm}]")
                elif op == "stp" or (base not in (t1, t2)):
                    if form < 0.85:
                        lines.append(f"{op} {size}{t1}, {size}{t2}, [x{base}, #{imm}]!")
                    else:
                        lines.append(f"{op} {size}{t1}, {size}{t2}, [x{base}], #{imm}")
            elif k < 0.80:
                size, r = rng.choice([("q", 16), ("d", 8), ("s", 4), ("h", 2), ("b", 1)])
                t = rng.randrange(32)
                lines.append(f"{rng.choice(['ldr', 'str'])} {size}{t}, [x{base}, #{r * rng.randrange(0, 16)}]")
            elif k < 0.86:
                idx = regs({base}, 1)[0]
                t = regs({base, idx}, 1)[0]
                lines += [f"mov x{idx}, #{8 * rng.randrange(0, 16)}"]
                lines.append(f"{rng.choice(['ldr', 'str'])} x{t}, [x{base}, x{idx}{rng.choice(['', ', lsl #3'])}]")
            elif k < 0.92:
                lines.append(f"str x{base}, [x{base}, #{8 * rng.randrange(0, 16)}]")  # the base itself
            elif k < 0.96:
                lines.append(f"ldr x{base}, [x{base}, #{8 * rng.randrange(0, 16)}]")  # into its own base
                lines += setreg(base, addr())
            else:
                lines.append(f"add x{base}, x{base}, #{rng.randrange(0, 64)}")
    elif kind < 0.80:  # a prologue and epilogue
        sp = addr(16)
        frame = 16 * rng.randint(2, 8)
        lines += setreg(16, sp) + ["mov sp, x16"]
        lines.append(f"stp x29, x30, [sp, #-{frame}]!")
        saved = []
        for off in range(16, frame - 15, 16):
            a, b = rng.sample([19, 20, 21, 22, 23, 24, 25, 26, 27, 28], 2)
            if rng.random() < 0.3:
                lines.append(f"stp d{rng.randrange(8, 16)}, d{rng.randrange(8, 16)}, [sp, #{off}]")
            else:
                lines.append(f"stp x{a}, x{b}, [sp, #{off}]")
                saved.append((a, b, off))
        lines.append(f"add x{rng.randrange(0, 16)}, x{rng.randrange(0, 16)}, #{rng.randrange(0, 255)}")
        for a, b, off in saved:
            lines.append(f"ldp x{a}, x{b}, [sp, #{off}]")
        lines.append(f"ldp x29, x30, [sp], #{frame}")
    else:  # a few loads and stores through two bases
        b1, b2 = regs(set(), 2)
        lines += setreg(b1, addr()) + setreg(b2, addr())
        for _ in range(rng.randint(2, 6)):
            b = rng.choice([b1, b2])
            t = regs({b1, b2}, 1)[0]
            lines.append(f"{rng.choice(['ldr', 'str'])} x{t}, [x{b}, #{8 * rng.randrange(0, 20)}]")
lines += ["svc #0", ""]
open(out, "w").write("\n".join(lines))
