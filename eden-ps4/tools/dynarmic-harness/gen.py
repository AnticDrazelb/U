# Random A32/Thumb programs of loads and stores of several registers, near page boundaries, for the
# single-walk differential test (dynbench test ... vs ... nosingle).
#   python3 gen.py <seed> <arm|thumb> <out.s>
import random
import sys

seed, mode, out = int(sys.argv[1]), sys.argv[2], sys.argv[3]
rng = random.Random(seed)
DATA, PAGES = 0x10000000, 4096
MARKED, UNMAPPED = 0x10800000, 0x10900000
thumb = mode == "thumb"


def addr():
    kind = rng.random()
    if kind < 0.15:
        page = MARKED
    elif kind < 0.25:
        page = UNMAPPED + rng.choice([-4096, 4096])  # either side of the unmapped page
    elif kind < 0.30:
        page = UNMAPPED
    else:
        page = DATA + 4096 * rng.randrange(16, PAGES - 16)
    off = rng.choice([rng.randrange(0, 96), rng.randrange(4096 - 96, 4096 + 96), rng.randrange(0, 4096)])
    return (page + off) & ~3


def setreg(r, value):
    return [f"movw r{r}, #{value & 0xFFFF}", f"movt r{r}, #{value >> 16}"]


def reglist(exclude, allow_lr=True, low_only=False):
    pool = [r for r in range(0, 8 if low_only else 13) if r not in exclude]
    if allow_lr and not low_only:
        pool.append(14)
    k = rng.randint(1, min(len(pool), 9))
    regs = sorted(rng.sample(pool, k))
    return "{" + ", ".join("lr" if r == 14 else f"r{r}" for r in regs) + "}"


lines = [".syntax unified", ".thumb" if thumb else ".arm", ".text", ".globl _start", "_start:"]
for _ in range(rng.randint(8, 24)):
    op = rng.random()
    base = rng.choice([r for r in range(0, 13) if r != 7] if not thumb else [r for r in range(0, 13)])
    if op < 0.35:
        lines += setreg(base, addr())
        mnem = rng.choice(["ldmia", "ldmdb", "stmia", "stmdb"] + ([] if thumb else ["ldmib", "ldmda", "stmib", "stmda"]))
        wb = rng.random() < 0.5
        regs = reglist({base} if (wb or mnem.startswith("ldm")) else set())
        if thumb and len(regs.split(",")) < 2:
            regs = "{r8, r9}" if base not in (8, 9) else "{r10, r11}"
        lines.append(f"{mnem} r{base}{'!' if wb else ''}, {regs}")
    elif op < 0.55:
        sp = addr() & ~7
        lines += setreg(12, sp) + ["mov sp, r12"]
        regs = reglist({12}, allow_lr=True)
        lines.append(f"push {regs}")
        lines.append(f"add r{rng.randrange(0, 12)}, r{rng.randrange(0, 12)}, #{rng.randrange(0, 255)}")
        lines.append(f"pop {regs}")
    elif op < 0.70:
        sp = addr() & ~7
        lines += setreg(12, sp) + ["mov sp, r12"]
        first = rng.randrange(0, 16)
        count = rng.randint(1, min(8, 16 - first))
        dl = "{" + ", ".join(f"d{first + i}" for i in range(count)) + "}"
        lines.append(f"vpush {dl}")
        lines.append(f"vpop {dl}")
    elif op < 0.85:
        lines += setreg(base, addr())
        mnem = rng.choice(["vldmia", "vstmia", "vldmdb", "vstmdb"])
        wb = mnem.endswith("db") or rng.random() < 0.5
        if rng.random() < 0.5:
            first = rng.randrange(0, 16)
            count = rng.randint(1, min(8, 16 - first))
            regs = "{" + ", ".join(f"d{first + i}" for i in range(count)) + "}"
        else:
            first = rng.randrange(0, 32)
            count = rng.randint(1, min(8, 32 - first))
            regs = "{" + ", ".join(f"s{first + i}" for i in range(count)) + "}"
        lines.append(f"{mnem} r{base}{'!' if wb else ''}, {regs}")
    else:
        lines += setreg(base, addr())
        d = rng.randrange(0, 16)
        lines.append(f"{rng.choice(['vldr', 'vstr'])} d{d}, [r{base}, #{4 * rng.randrange(0, 4)}]")
lines += ["svc #0", ""]
open(out, "w").write("\n".join(lines))
