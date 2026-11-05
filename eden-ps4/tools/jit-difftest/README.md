# jit-difftest: the 64-bit translator against itself

Random blocks of A64 code (integer, pointer and memory instructions; `all`: every instruction the
decoder knows) run three times from the same state: as the PS4's CPU Auto translates them (`on`),
without the IR passes (`off`: get/set elimination, constant propagation), and with no optimisation
at all (`bare`, CPU accuracy Paranoid, the referee). Any difference in registers, flags, PC or
memory written is a mistranslation; each one is shrunk to the fewest instructions that still show it
and printed once per kind, disassembled (llvm-mc).

    jit-difftest [iterations] [seed] [int|all] [only-getset|only-constprop]
    DYNARMIC_HOST_FEATURES_MASK=16b00f jit-difftest ...   # the PS4's Jaguar: no BMI2, AVX2, FMA, AVX-512
    jit-irdump <hex instruction>...                       # the IR, passes on and off

Build: fmt and xbyak installed somewhere (`cmake --install` of eden-deps' copies), then the
CMakeLists.txt here (its comment). Memory goes through the callbacks (no fastmem, no page table).

What it found (04.68, patch 0152), both in blocks where the passes read a W register at its first
use: the PS4's sign-extended W register reads lost their upper half when the register allocator
moved them (SMADDL, SMULL, SXTW of a negative value: Little Kitty, Big City's heap), and Eden's
32-bit SDIV by zero gave the dividend instead of 0. After the fixes: 0 differences in 200,000 integer
blocks with all host features and 200,000 with the Jaguar's. (`all` stops at instructions the
Switch's Cortex-A57 doesn't have, FP16 among them, which the translator asserts on: not used by
games.)
