# How the Switch emulators translate ARM code, next to ours (2026-11-07)

Follow-up, beyond the Switch: [arm-to-x86-translators.md](arm-to-x86-translators.md) (Berberis,
melonDS, DeSmuME, MAME, QEMU; Dolphin and Cemu for one technique each).

The question: how does every public Switch emulator run the game's ARM code, what do we do
differently on the PS4, which is more effective, and what can we take from them.

Read from source where it was public: Ryubing's continuation of Ryujinx
(`git.ryujinx.app/ryubing/ryujinx`, commit `a61a426`: `src/ARMeilleure`, `src/Ryujinx.Cpu`), Eden
upstream (`git.eden-emu.dev/eden-emu/eden`, `885b333`, 2026-11-06: `src/dynarmic`), dynarmic's
continuation (`github.com/azahar-emu/dynarmic`), Suyu's final release (`github.com/suyu-emu/suyu-v0.0.4`:
`src/core/recompiler`, `src/core/arm/recomp`), Strato (`github.com/strato-emu/strato`: `skyline/nce`)
and nx-on-orbis (`github.com/alechurri/nx-on-orbis`). Citron's server wasn't reachable; Citron,
Sudachi and Torzu are Yuzu forks on dynarmic like Eden.

## The short answer

- **The PS4 is an x86-64 machine, so only two designs are directly comparable:** dynarmic (what
  Eden, Citron, Sudachi and the other Yuzu forks use, and what ours started from) and Ryujinx's
  ARMeilleure. Suyu's final release adds a third, an ahead-of-time recompiler. Everything else
  (native execution on ARM, Apple's hypervisor, Ryujinx's LightningJit) needs an ARM64 host.
- **Ours is well ahead of every other dynarmic.** Eden upstream's dynarmic has changed only by bug
  fixes and asserts since the commit our patches start from (29 files, a handful of lines).
  Dynarmic's own continuation is just as quiet: a few vector tweaks and an identity-removal pass in
  2026. Our translator patches (47 named dynarmic, plus the fastmem and native-function ones in
  Eden's core) aren't in any of them.
- **Ryujinx's ARMeilleure is architecturally stronger in one big way:**
  - It compiles hot code a whole guest function at a time.
  - It keeps guest registers in host registers across every block of the function, loops included.
  - It turns guest calls and returns into real host `call`/`ret`.
  - That's exactly where our measured overhead is: about a fifth of the host instructions in the
    Unity games' hot code are guest-register loads and stores at block edges, and another 16% are
    branches and links.
- **Nothing to copy as-is:** Ryujinx is C# and can't run on the PS4, and its ideas would have to be
  rebuilt inside dynarmic. The ideas are what's usable (last section).

## Who uses what

| Emulator | CPU on an x86-64 host | CPU on an ARM64 host | State (2026) |
|---|---|---|---|
| **Ours (New Eden for PS4)** | dynarmic + our translator patches (47 dynarmic ones, fastmem and native functions in the core) | - | active |
| Eden (upstream) | dynarmic, unchanged since our base | dynarmic's ARM64 backend, NCE on Android | active, self-hosted |
| Citron, Sudachi, Torzu | dynarmic (Yuzu's) | dynarmic, NCE | active / quiet |
| Yuzu, Suyu (emulation mode) | dynarmic | dynarmic, NCE | ended |
| Suyu 0.0.4 recompiler mode | game code translated ahead of time to C, then compiled; dynarmic for code it missed | - | final release, ended |
| Ryujinx / Ryubing | ARMeilleure | LightningJit, Apple Hypervisor on macOS | Ryubing active, self-hosted |
| Kenji-NX, MeloNX | (Ryujinx forks for Android, iOS) | LightningJit / native | active |
| Skyline, Strato | - | NCE only (the game's code runs natively, its system calls and some functions patched) | Strato quiet |
| Hydra, nxnative | - | native on Apple silicon (hypervisor / rewritten code) | active |
| nx-on-orbis (PS4) | Eden's dynarmic, PS4 setup only; fastmem on an experimental branch (an 8 GiB view; races ran out of memory) | - | last commit 2026-10-07 |

## The three x86-64 translators side by side

| | **Ours** | **Ryujinx ARMeilleure** | **Suyu recompiler** |
|---|---|---|---|
| Unit of translation | a block; superblocks: forward CBZ/TBZ/B.cond as side exits (04.28-29), backward branches to the block's own start (loops, 04.81); short leaf calls and PLT stubs followed into the caller (04.74, 04.81) | a **whole function** (all blocks reachable by branches, loops included, up to 2,500 instructions); calls are separate functions | a block, one C function each |
| Tiers | one (dynarmic's compiler, made ~6x faster in 04.71) | **two**: every function first compiled quickly (no optimizer, simple allocator, 500-instruction limit); after **100 calls** recompiled on a background thread with SSA form, an optimizer and linear-scan register allocation | none: all ahead of time |
| Guest registers | in host registers within a block, written back at its end; the loop's busiest register pinned in loops (04.73) | in host registers across the **whole function**; written back only around calls and at exits (`RegisterUsage`) | in a memory context; the C compiler keeps them in registers within a block |
| Flags (NZCV) | kept in the host's flags; dead ones dropped, including a look-ahead across blocks (04.02, 04.23) | ordinary values in the function, dead ones removed by the optimizer | always stored to memory by a helper |
| Calls and returns | block exits; a 256-entry emulated return stack, then an indirect jump (04.24, 04.27); known returns followed (04.74) | guest `BL` is a host **`call`** to the target's code (through a function table), guest `RET` a host **`ret`**, the caller checking the address it came back to | `RET` and conditional branches go back to a dispatcher; unconditional jumps to known blocks are direct calls |
| Indirect branches | guarded links (04.73), the dispatcher otherwise | a one-level table lookup inline, the dispatch stub otherwise | the dispatcher |
| Memory | **fastmem on the PS4** (direct-memory aliasing, guest pages lined up with the PS4's 16 KiB pages, 2 MiB blocks, thread-local storage too: 03.84-04.80), inline page walk where declined | host-mapped (the same idea as fastmem) or an inline software page table | an inline page-table lookup in C, a call otherwise |
| Exclusives, barriers | inline exclusives through the page table (03.80), plain LDAR, DMB LD/ST dropped (04.25) | through its monitor | calls |
| Library functions | the game's memcpy, memmove, memset, memcmp run natively (04.75, 04.80) | translated | translated |
| Persistent cache | the translation cache on disk (04.17-04.18) | PPTC: the second tier's code on disk with relocations, loaded on boot in parallel | everything is ahead of time |

### Where ours is ahead

- **Memory on the PS4.** Nobody else runs fastmem on a PS4: nx-on-orbis's attempt stays on a branch.
  Ours maps guest memory into a 2^39 arena despite the 16 KiB host pages. That needs stacks and
  thread-local pages lined up, padding, and 2 MiB blocks.
- **Peephole and code generation work** none of the others have: flags kept in host flags across
  blocks, constants in one load, float and sign-extending loads in one instruction, bitfields as
  shifts, inline exclusives, plain acquires.
- **Native library functions and PLT-stub following.** Strato hooks some game functions the same
  way on ARM, but none of the x86 translators do it.
- **Compile speed.** One tier, ~6x faster since 04.71, plus the disk cache. ARMeilleure's quick tier
  costs it first-run speed, and its second tier needs spare threads, which the PS4 has (cores 4-6
  are mostly idle).

### Where Ryujinx's design is ahead

- **Registers across blocks.** Our hot code pays guest-register loads and stores at every block
  edge: 19-21% of host instructions in the Unity games' hot blocks (04.72's Little Kitty analysis). 17% of their
  samples were loops paying a block exit and entry each time round (since cut by 04.81's backward
  superblocks for single-block loops). A third of translated-code samples land in a block's first
  16 bytes. A function-level unit with one register allocation over it removes most of this.
- **Returns predicted by the hardware.** Our return is a lookup in the emulated return stack, then
  an indirect jump. The Jaguar predicts indirect jumps by branch history, and a function returning
  to many callers mispredicts often. A real `ret` uses the CPU's return stack, which predicts
  returns nearly perfectly. How much this costs us hasn't been measured (below).
- **Optimizing only what's hot, on another thread.** A second tier spends compile time only on the
  ~1% of functions that run most, and off the game's threads.

### Suyu's recompiler

Its comments report 1.80 ms a frame against dynarmic's 2.905 on its test. That's against Eden's
stock dynarmic, not ours: our A64 test program `game.c` went from 3.34 to 1.81 million host
instructions (-46%) between 04.22 and 04.30 alone. Its code keeps registers and flags in memory and reaches memory through a page-table lookup,
so its gain comes from the C compiler optimizing within each block. For the PS4 it would need a PC
tool run per game and a huge compiled binary per game, plus the JIT anyway for the code it misses.
Our disk cache already removes compile time on later runs. Not worth pursuing.

### Not usable on the PS4

Native execution (NCE in Eden/Yuzu/Skyline/Strato), Apple's hypervisor (Ryujinx, Hydra) and
nxnative's rewritten code all run the game's ARM code on an ARM CPU. LightningJit generates ARM64
code only. They show nothing for an x86 host except the general point: the less translation there
is between the game's code and the CPU, the better.

## How to use this

Ranked by expected gain for the CPU-bound games (Little Kitty, Bluey, Silksong, Mario Kart's main
thread). None of this helps Breath of the Wild, which is held back by Eden's GPU thread (04.84).

1. **Measure first, in the harness, before building anything** (a day).
   - On the desktop harness (`tools/dynarmic-harness`, with the Unity-shaped `unity64` and game-like
     `big` programs), count per guest instruction: host instructions at block edges, block entries,
     and with `perf stat` (or callgrind's branch simulation) indirect-jump and return mispredicts.
   - That sizes the next two items: if return mispredicts are rare, item 3 is pointless; if edge
     loads and stores dominate, item 2 is the one.
2. **A second tier for hot code: guest registers kept across blocks** (weeks; the large one).
   - Count block entries (dynarmic can already). A block run more than N times gets its region
     recompiled on an idle PS4 core (4-6): the blocks reachable from it by branches, as
     ARMeilleure's high tier does.
   - Use one register allocation over the region, guest registers written back only at its exits
     and around calls. The new code is swapped in when ready, and the first tier stays as it is.
   - Our superblocks are halfway there: they already put several blocks in one IR, but only as
     straight lines with side exits. Joins inside a region need phi nodes or a fixed guest-to-host
     register mapping inside it.
   - Estimate: a large part of the ~20% edge instructions in hot code, perhaps 5-15% on the
     CPU-bound games. Risk: high (a register mapping bug is a wrong value, rarely a crash). The
     differential test generators (gen5-gen18) are what makes it testable.
3. **Guest calls and returns as host `call`/`ret`** (a week or two, only if item 1 shows return
   mispredicts).
   - `BL` calls the target block's code and `RET` returns; a check after the call guards against
     the guest returning somewhere else (longjmp, threads switched).
   - A call depth limit keeps the host stack bounded, as ARMeilleure does with its
     `EmitCallDepthCheckAndIncrement`.
   - Risk: medium (stack balance across exceptions and the PS4's fixed thread stacks).
4. **PPTC-style relocations for the disk cache** (only if boot times matter). Already looked at in
   [ryujinx-comparison.md](ryujinx-comparison.md) item 5. Our cache works, so this isn't a speed item.

Nothing from Eden upstream, dynarmic's continuation, Suyu, Strato or nx-on-orbis is worth porting
into our translator now.
