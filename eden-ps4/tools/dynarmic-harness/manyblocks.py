# A large ARM program for translation cost: many functions with compares and branches, each calling
# leaf functions far away in the image (a game's code: thousands of blocks, calls across megabytes).
# Run once, its time is mostly the recompiler's. 04.02's dead flags registered every block's
# look-ahead as one range from its lowest to its highest address read: across a far call, megabytes,
# and the invalidation map's sets grew with every block (Mario Kart: 1.3 ms a block, no frame).
#   python3 manyblocks.py <functions> <out.s>
import random
import sys

n, out = int(sys.argv[1]), sys.argv[2]
rng = random.Random(1)
leaves = 64
lines = [".syntax unified", ".arm", ".text", ".globl _start", "_start:", "mov r0, #0", "mov r1, #1"]
for i in range(n):
    lines += [f"bl F{i}"]
lines += ["svc #0"]
for i in range(n):
    lines += [".p2align 2", f"F{i}:", "push {r4, lr}"]
    for k in range(4):
        lines += [f"cmp r0, #{rng.randrange(0, 256)}", f"beq F{i}_{k}", f"add r1, r1, #{k + 1}",
                  f"bl G{rng.randrange(0, leaves)}", f"F{i}_{k}:", f"tst r1, #{1 << k}", f"addne r0, r0, #1"]
    lines += ["pop {r4, pc}"]
# the leaves, far from the callers: a megabyte of padding first
lines += [".p2align 12", ".space 1048576"]
for g in range(leaves):
    lines += [".p2align 2", f"G{g}:", "cmp r1, #3", "movne r2, r1", "add r1, r1, r2", "bx lr"]
open(out, "w").write("\n".join(lines) + "\n")
