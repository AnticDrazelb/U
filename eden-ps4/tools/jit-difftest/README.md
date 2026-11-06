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

## Since 04.69

Memory as on the PS4: a fastmem arena (every 4th page of a 2 MiB window left out, as pages the PS4
declines), the page table of absolute offsets (inline and shared walks), exclusives; each block run
twice from the same state (the second run is what the PS4 runs once a declined page is known).

    jit-difftest <states> <seed> blocks <file>        # real blocks: lines of "<start> <hex words>..."
                                                      # (the console's 'jit dump', EDEN_PS4_DUMP_RANGE)
      DIFFTEST_FORK=1     each state in a child process (a crash costs that state only)
      DIFFTEST_SHRINK=1   a failing block shrunk to the fewest instructions that still fail
      DIFFTEST_TRACE=1    the shrunk block's callback accesses, passes on and bare
      DIFFTEST_DUMP=1     every block's IR, as translated and as emitted (stderr)
    jit-difftest <blocks> <stride> compile <nro> [1]  # the translator's speed: blocks every <stride>
                                                      # bytes of the NRO's code, translated, not run
      DIFFTEST_PROFILE=1  where the time goes (sampled, addr2line)
      DIFFTEST_HASH=1     a hash of what was emitted (setarch -R, the same environment: comparable)

What 04.71 found with them: Little Kitty, Big City's block 0x80a7f23c (from 04.70's on-console
search) wrong in 141 of 300 states with the Jaguar's features only; shrunk to 11 instructions, a
store went to a host address: `EmitFPFixedU64ToSingle` without AVX-512 took a scratch register
inside one branch of its sign test, and the allocator's move of the block's SP (get/set
elimination keeps it in a register) happened on that path only (patch 0155). And the translator's
time: 0.35 s of decoder table set-up per process and ~33 us a block, now under 1 ms and ~14 us,
the same code out (patch 0156).


## 04.73

More of the PS4's configuration, and every run checked:

    DIFFTEST_NO_TICKS=1   no cycle counting (the PS4's multicore): superblocks' side exits and links
                          without the halt check apply; code outside the block reads as SVC #0, which
                          halts; random forward branches (CBZ/TBZ/B.cond/B) are mixed in
    DIFFTEST_FULL=1       every run compared with bare, not only those where on and off differ (on
                          and off share the PS4 extras, loop pins, followed branches: checked too)
    DIFFTEST_CALLS=1      a third of the blocks call a leaf function (BL there, RET back)
    DIFFTEST_FOLLOW=1     BL/forward B translated through, known returns followed
    DIFFTEST_NO_ABI=1     no reliance on the call standard's dead flags (random code breaks it)
    DIFFTEST_SAFE=1       Auto's unsafe float shortcuts off (exact against bare)
    DIFFTEST_NO_EXTRAS / NO_PINS / NO_REUSE=1   the PS4 extras / loop pins / stack range-check reuse off
    jit-difftest <n> <seed> loops [all]          random loop bodies, pins on against off against bare
    jit-difftest 0 0 loopbench                   a few loops timed with pins and without
    DIFFTEST_PLT=1        half of the blocks (those without DIFFTEST_CALLS's call) make two calls
                          through a GOT slot in the window, each through a PLT stub (BL to ADRP-like
                          MOVZ/MOVK X16, LDR X17, [X16, #off], ADD X16, BR X17) or inline (BLR X17);
                          the first callee may point the slot at another one (the guarded link's check
                          failing), a slot holds garbage now and then (guarded links, 04.73)
    DIFFTEST_NO_GUARD=1   the guarded links off
    DIFFTEST_NO_LEAF=1    BLs to short leaf functions not followed (04.74's follow_leaf_calls, on here)
    DIFFTEST_NATIVE=1     every block: X0-X2 set, BL to a real memcpy/memmove/memset/memcmp (byte loops;
                          memcmp's first differing bytes' difference, as the SDK's; 04.80), the
                          registers the callee may change cleared: run natively (on, off; 04.75's
                          native_functions) against the real code (bare); DIFFTEST_NO_NATIVE=1: not.
                          The last line counts the native calls. (difftest32: the same, ARM.)
    DIFFTEST_WITHOUT=<extra>   one of the PS4 extras off (fold_address_offsets, elide_dead_nzcv_abi, ...)
    DIFFTEST_PROGRESS=1   each iteration's code (stderr) before it runs, and which run: to find a hang
    DIFFTEST_SKIP=<n>     the first n iterations made but not run, to get to one
    DIFFTEST_ONLY=on|off|bare   each iteration's one run alone, no comparison (a hang everyone's
                          is the program's: a callee's random code making X30 its own address)
                          (difftest32: DIFFTEST32_ONLY; DIFFTEST_PLT there too, LDR PC stubs, BLX
                          through the GOT, ARM and Thumb callees, for the 32-bit guarded links)
    jit-difftest 0 0 pltbench [rounds]           a PLT stub call and a GOT call timed, guard on and off
                                                 (PLTBENCH_GUARD=0/1: one of them, for callgrind)

Real code (04.73's last check): `blocks` over 5,934 32-instruction blocks cut from the homebrew NROs,
the PS4's configuration, no cycle counting: 5,819 the same, 110 unclear (on and off differing from
bare alike: Auto's unfused multiply-adds), 5 the optimised translation wrong, and all 5 are the
calling convention's dead flags (`DIFFTEST_WITHOUT=elide_dead_nzcv_abi`: the same): the block
calls or returns with flags it set but didn't read, which the callee or caller never reads.

(04.74) `difftest32` has the PS4's page table (absolute offsets over its flat arena: the walks of
accesses fastmem was declined for), its random code writes no PC, an iteration has 5 seconds, and the
last line counts the iterations where on and off differed at all.

`difftest32.cpp` (`jit-difftest32 <n> <seed> [loops]`): the same for the 32-bit translator, random
ARM, VFP and ASIMD blocks with the 32-bit PS4 extras (on), without (off, Auto as in 04.38-04.72) and
bare, a 4 GiB fastmem arena with no access past it (as the PS4's), no cycle counting;
`DIFFTEST32_WITHOUT=<extra>` leaves one extra off (to tell which one a difference is). Build it as
jit-difftest (another `add_executable` with tests/fuzz_util.cpp).

Instruction generation follows the seed (not fuzz_util's own generator), so a failure reproduces.
