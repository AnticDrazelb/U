# ARM-to-x86-64 translators beyond the Switch (2026-11-07)

The follow-up to [arm-translation-comparison.md](arm-translation-comparison.md): how other projects
run ARM code on x86-64 CPUs, not only Switch emulators, and what that means for ours.

Read from source:

- Google's Berberis (`android.googlesource.com/platform/frameworks/libs/binary_translation`);
- melonDS (`src/ARMJIT*`);
- DeSmuME (`arm_jit.cpp`);
- MAME's ARM7 recompiler (`arm7drc`, `drcfe`, `drcbex64`);
- QEMU's TCG (`accel/tcg`, `target/arm/tcg`, `tcg/`).

Two x86-64 JITs whose guest isn't ARM, for one technique each: Dolphin's JIT64 (GameCube/Wii,
PowerPC) and Cemu's recompiler (Wii U, PowerPC, which this repo once ported to the PS4:
`patches/cemu-ps4.patch`). Checked: Azahar, Panda3DS and Vita3K's CPU cores. Not readable: Intel's
Houdini (closed; a 2021 Black Hat talk describes it).

## The short answer

- **Most ARM emulators on x86 use dynarmic itself.** Azahar (the 3DS emulator continuing Citra),
  Panda3DS (3DS) and Vita3K (PS Vita) all build on it, like the Switch's Yuzu family. Ours is
  dynarmic's most developed branch (the Switch survey).
- **The independent designs** are Google's Berberis (RISC-V to x86-64 upstream; a third-party
  project, Digitalis, adds ARM64 to it), melonDS's and DeSmuME's DS JITs, MAME's ARM7 recompiler and
  QEMU. Each has one or two ideas we don't.
- **Three ideas recur in every design faster than a plain block JIT, and ours lacks all three:**
  1. Hot code recompiled with guest registers kept in host registers across blocks: Berberis's
     second gear, Ryujinx's second tier, Cemu's whole-function compiles.
  2. Every guest register a loop uses kept in a host register for the whole loop: Berberis has a
     pass for exactly this. It's the small version of 1; ours keeps one register (04.73).
  3. Guest calls and returns as host `call`/`ret`, so the CPU's return predictor does its job:
     Ryujinx and Dolphin.
- **Two small ones:**
  - Loads from read-only memory as constants (melonDS). Dynarmic has a pass for 32-bit code that
    Eden never enables, and none for 64-bit.
  - Busy-wait loops detected and skipped (melonDS, from Dolphin).

## Side by side

| Translator (guest → host) | Unit | Guest registers across block edges | Calls and returns | Flags | Memory | Tiers |
|---|---|---|---|---|---|---|
| **Ours** (ARMv8/v7 → x86-64) | block; superblocks (forward side exits, backward to its own start); short leaves and PLT stubs followed | written back at each block end; one pinned in loops (04.73) | emulated return stack (256), indirect jump | in host flags, dead ones dropped, look-ahead across blocks | fastmem on the PS4, inline page walk | one, plus a disk cache |
| **Berberis** (RISC-V → x86-64; ARM64 via Digitalis) | region: a straight stretch of up to 200 instructions, branches inside it internal | first gear: per block; **second gear: one register allocation over the region**, guest-context loads and stores **moved out of loops** | through the dispatcher | as values, folded into the host's (`FoldWriteFlags`) | direct (Android's own process memory) | **interpreter, a lite translator that counts runs, a heavy optimizer for hot regions** |
| **melonDS** (ARMv5/v4 → x86-64, ARM64) | block of up to 32 instructions, **static branches followed** | written back at block end | through the dispatcher | as needed | **fastmem** (fault handler) | one; **idle loops skipped**; **literal loads as constants** |
| **DeSmuME** (ARMv5/v4 → x86-64) | block | AsmJit's allocator within the block | through the dispatcher | as needed | calls, by address class | one |
| **MAME ARM7 DRC** (→ its UML → x86-64) | a sequence that follows branches | its UML allocator within the sequence | through the dispatcher | **backward liveness**: only flags a later instruction needs | calls | one |
| **QEMU TCG** (any → any) | translation block (up to 512 instructions) | written back at block end and around helpers | blocks **chained** directly; indirect through an inline hash lookup | N, Z, C, V as four separate values, computed lazily | software TLB (system), direct base (user mode) | one |
| **Dolphin JIT64** (PowerPC → x86-64) | block, with branch following | per block (a register cache) | **`bl` is a host `call` with the expected return pushed; `blr` a host `ret` after a check**; a guard page catches host stack overflow | per block | fastmem with backpatching | one |
| **Cemu** (PowerPC → x86-64) | **a whole function** (`PPCFunctionBoundaryTracker`) | **one register allocation over the function** (live ranges across segments) | within a function: jumps | per function | direct | interpreter first, then the recompiler on its own thread |
| **Houdini** (ARM → x86, Intel, closed) | - | - | - | - | - | per the 2021 research: no x86 code generated at run time, an interpreter |

## What each one shows us

- **Berberis is the closest modern design to what we'd build.**
  - The first gear translates fast and counts each region's runs. Past a threshold, the heavy
    optimizer recompiles the region: critical edges removed, cold blocks moved to the end, guest
    registers moved out of loops (`RemoveLoopGuestContextAccesses`), dead code and redundant writes
    removed, flags folded, then one register allocation.
  - A region is just a straight run of up to 200 instructions with its internal branches, not a
    discovered function, which is far simpler to build than Ryujinx's or Cemu's function discovery.
- **melonDS** has the console-emulator tricks:
  - Following static branches into the same block (our superblocks do this).
  - Idle-loop skipping: a loop with no stores and no dependence between iterations is a wait, so
    its time is skipped.
  - Literal loads from code memory as constants.
  - Fastmem.
- **MAME** computes, backwards over each instruction sequence, which registers and flags later
  instructions need, and skips the rest. That's our dead-flag look-ahead, done for registers too.
- **QEMU** is the general-purpose baseline. It's portable and correct, but slower than dynarmic for
  games: registers and the four flag values are written back at every block end, and in system
  mode every access goes through a software TLB.
- **Dolphin** is the strongest evidence for call/return mapping. Its JIT64 has done it for years
  (`m_enable_blr_optimization`) with a guard page against host stack overflow; Ryujinx does the
  same with a call-depth counter.
- **Cemu** shows whole-function compiling with global register allocation on x86-64, on a
  separate thread, and this repo's earlier Cemu-on-PS4 work ran it on the Jaguar.
- **Opposite direction, not reviewed:** Rosetta 2, FEX and Box64 run x86 code on ARM.

## What this changes in our plan

The Switch survey's plan stays. This adds a smaller step before its big one, and two small items.

1. **Measure in the harness first** (unchanged): host instructions at block edges, return
   mispredicts. Add: guest-register loads and stores inside loops (`gcmark64`, `unity64`).
2. **New, smaller than a second tier: every guest register a loop uses kept in host registers**
   (Berberis's loop pass).
   - Extend 04.73's loop pins from the loop's busiest register to all the loop reads and writes, as
     many as host registers allow.
   - Loads go before the loop, stores at its exits.
   - Our backward superblocks (04.81) already make a single-block loop one unit, so this is a
     change to the pinning, not a new compiler.
   - Days, not weeks; measurable on the GC loop and the Unity programs; the differential tests
     cover it.
3. **Guest calls and returns as host `call`/`ret`**, if the measurement shows return mispredicts:
   Dolphin's way, with a guard page against the host stack overflowing, or Ryujinx's depth counter.
4. **A second tier for hot regions**, the large one. Berberis's straight-run regions are a simpler
   unit than whole functions and a better first version.
5. **Small:**
   - Constant loads from read-only memory. Eden doesn't report read-only memory (`IsReadOnlyMemory`
     is never overridden), so dynarmic's pass for 32-bit code never runs; 64-bit code needs a pass
     of its own. GOT loads would become constants.
   - Idle-loop skipping only if a profile shows the main thread spinning; Switch games mostly wait
     in system calls.
