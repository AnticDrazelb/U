# Switch CPU Bench: can a PS4 run Nintendo Switch code?

Stage 1 of a feasibility study for running a Switch emulator on the PS4. Eden is the C++ successor
of yuzu, with a FreeBSD port; ruzu is a Rust rewrite with no PS4 target. Before porting any of
Eden, this answers the two questions that decide whether a port is worth doing:

1. **CPU.** How fast does the PS4's Jaguar run Switch (AArch64) code through Eden's recompiler,
   dynarmic? It is configured the way Eden configures it on a host without fastmem, which the PS4
   can't have (it can't map the same memory twice): an absolute-offset page table, the
   misalignment checks, and Eden's CPU accuracy settings.
2. **Memory.** Can the PS4 give it what Eden needs? That means 4 GiB of Switch DRAM as one buffer,
   a page table for a 39-bit address space, and executable memory for the code caches.

**Results from a PS4: [RESULTS.md](RESULTS.md).**

It is one app (`out/SwitchCpuBench.pkg`, title ID SWBN00001; version in `VERSION`) that takes 3-5
minutes. A status
line at the bottom of the screen shows what is running. The results stay on screen and go to
`/data/switch-bench/results.txt`. It writes nothing outside `/data/switch-bench/`; if it crashes,
Crema's runtime writes `crash.log` and `boot-trace.log` there.

## The start page

The app opens on a start page:

- **X** runs everything (3-5 minutes).
- **SQUARE** runs only the CPU tests.
- **TRIANGLE** runs only the memory checks.

If the last run didn't finish, the page first shows what that run left behind:

- the end of its `results.txt`
- its crash report from `crash.log`, compacted: the signal, `rip`, registers, code addresses
  found on the stack and the threads
- the last lines of its boot trace, where every step of a run is recorded before it starts

With a controller connected, the page then waits for a button, so a photo of it can come first.
That photo is enough to see where a run stopped. Stack addresses are ELF addresses: symbolize
them with `llvm-symbolizer --obj=<that build's ELF> 0x...`, so keep the ELF of every build
handed out. Without a controller, or after a run that finished, everything runs after a
countdown.

The last run's files are kept as `results-previous.txt` and `crash-previous.log`.

## What it runs

Seven small kernels (`guest/bench.c`), each built twice. One build is for AArch64 (Cortex-A57, the
Switch's core); that code runs through dynarmic out of emulated memory. The other is native x86-64
(Jaguar), as the reference:

| kernel | stands for |
|---|---|
| int | integer math, multiplies, divides |
| branchy | game logic: an entity state machine over an array |
| calls | recursion and indirect calls through a table |
| memory | pointer chasing over 4 MiB plus a sequential scan |
| copy | 16-byte block copies |
| float | vectorised float loops (NEON on the Switch) |
| fma | fused multiply-adds (the Jaguar has no FMA instruction) |

Each recompiled kernel runs under Eden's three CPU accuracy settings: Accurate, Auto (the
default) and Unsafe. Its checksum must equal the native one. Auto and Unsafe may differ only on
`fma`, on a CPU without FMA, where they compute multiply-adds unfused. Then three cores run at
once (Switch games use three), and the memory probes run.

## Reading the results

- **x N** is how many times slower than native code on the same CPU. Auto is what Eden would
  use. The summary line gives the geometric mean over the kernels.
- **Auto MIPS** is millions of guest instructions per second. A Switch core runs at 1020 MHz, and
  an A57 retires about one instruction per clock on code like this. So about 1000 MIPS keeps up
  with one Switch core that is busy 100% of the time; games rarely keep all three that busy.
- **first run** is the first Accurate run, including recompiling the kernel.
- **three cores at once** is each core's speed relative to running alone. The PS4's two Jaguar
  modules each have their own L2 cache.
- **slow-path memory accesses** (if shown) count loads and stores that didn't go through the
  page table. Each one is a C++ call, roughly 10 to 100 times slower. There should be none.
- **memory** lists what the system can still give, each pool asked directly:
  - direct memory, in all and free for the app
  - whether 4 GiB of it maps as one CPU buffer, the Switch's DRAM
  - free flexible memory (regular and system pools), and which pool can hold a 1 GiB page table
  - executable memory for code caches, from the system pool and from direct memory, up to 1.5 GiB
    each, and whether it really runs code: on some firmware (11.00) RWX mappings succeed but
    don't execute, which Crema's runtime found the hard way

## An Eden bug this found (fixed in `patches/dynarmic-ps4.patch`)

On FreeBSD, and so on the PS4, host memory sits below 2^39. There Eden enables dynarmic's
`page_table_sign_extension` (`src/core/arm/dynarmic/arm_dynarmic_64.cpp`). The A64 x86-64 backend
then emits `shl page, bit; sar page, bit` with bit = 57. That keeps only bits 0-6 of the entry,
which the attribute mask has already cleared, so every lookup comes out 0. **Every load and store
of the game then takes the slow path.** The A32 lookup and the arm64 backend shift by `63 - bit`.

Measured on the host (Eden's settings vs fixed, slowdown against native):

| kernel | Eden today | fixed |
|---|---|---|
| branchy | x31 | x5 |
| calls | x51 | x8 |
| copy | x65 | x8 |
| float | x126 | x14 |

The patch makes the A64 lookup shift by `63 - bit` too. The sign bit must also be 55, the top
bit of the 44 pointer bits that `Common::PageTable::ATTRIBUTE_MASK` keeps. Eden passes
`SIGN_BIT` = 57, which is a no-op after the mask, so negative offsets would point to the wrong
memory. This benchmark passes 55; a port of Eden would change `SIGN_BIT`. Tested both ways on
the host: `BENCH_LOW_HOST=1` places guest memory below the guest addresses, which makes every
offset negative. All checksums match, with zero slow-path accesses.

The patch also adds a code allocator for the PS4. The PS4 only makes memory executable if it is
mapped executable to begin with: `mprotect` can't add `PROT_EXEC`, and Crema's runtime serves such
mappings from a pool that can run code. xbyak instead maps code memory read/write and then
re-protects it. The patch maps it read/write/execute once (`backend/x64/xbyak.h`).

## Building

Needs:

- Eden: git.eden-emu.dev/eden-emu/eden (or its GitHub mirror eden-emulator/mirror), commit
  `eb88a40`; only `src/` is used. Apply
  `patches/dynarmic-ps4.patch` with `patch -p1` from Eden's root.
- xbyak v7.40.1
- Crema's PS4 sysroot and toolchain (`docs/BUILDING.md`), which supply the Boost and fmt headers
  and the PS4 build; clang 21 builds the AArch64 guest.

```
EDEN_DIR=~/eden XBYAK_DIR=~/xbyak switch-bench/build.sh host   # desktop check: ~/sb-host/switch_bench
EDEN_DIR=~/eden XBYAK_DIR=~/xbyak switch-bench/build.sh ps4    # out/SwitchCpuBench.pkg
```

The PS4 build reuses Crema's runtime: heap, mmap and thread wraps, crash log. It compiles its
own copies of those files with the log folder moved to `/data/switch-bench`, so Crema's logs in
`/data/Crema` are never touched.

Host test knobs:

- `BENCH_SCALE=0.1` makes a quick run.
- `BENCH_SIGN_BIT=57` uses Eden's sign bit.
- `BENCH_LOW_HOST=1` gives negative page-table offsets.
- `-DBENCH_HOST_RWX_ALLOCATOR=ON` uses the PS4 code allocator.

Validated on the host:

- Under ASan, UBSan and LeakSanitizer: no reports, except one in Boost's `stable_vector`
  (dynarmic's IR blocks, upstream).
- Built with `-march=btver2` (as for the PS4) and run under QEMU with the Jaguar's instruction
  set (AVX and F16C; no FMA, AVX2 or BMI2), so dynarmic takes the code paths it will take on the
  PS4: all checksums correct, the FMA fallback included.
- The PS4 screen code, built against stand-ins for the video-out and controller calls, rendered
  to an image: the results screen, and the start page with leftovers in the runtime's own
  `crash.log` and boot-trace formats.

## What the consoles allow

On a console, 01.00 to 01.02 were ended in their first `sceVideoOutOpen`, before their first
frame, with error CE-34878-0 and no crash.log (the boot trace's last line was the step before
it). Crema's frontend says why: "calling into a system library whose module isn't loaded kills
the app silently". Since 01.03 the program loads the module of every system library it calls
before anything else, as Crema's `LoadSystemModules` does: SystemService, UserService, VideoOut,
Pad, Net. shadPS4 couldn't show this: it implements those libraries without loading anything.

The program also imports only functions that Crema imports: the screen is set up as Crema's
launcher does it (three linear A8B8G8R8 frames in write-combined direct memory, a flip event
queue), there are no system notifications, and `mprotect` goes to `sceKernelMprotect`
(Crema's).
To check after a change:

```
llvm-nm -u <Crema ELF> | awk '{print $2}' | sort -u > crema.txt
llvm-nm -u ~/sb-ps4/switch_bench | awk '{print $2}' | sort -u | comm -13 crema.txt -   # must print nothing
```

Every step of the startup (each video-out call, the controller) is in the boot trace, which
PS4-Xplorer and similar file managers can open on the console.

## Testing in shadPS4

shadPS4 (github.com/shadps4-emu/shadPS4), a PS4 emulator that runs on Linux, catches ordinary
bugs before a build goes to the console. It replaces Sony's libraries and the PS4 kernel with
its own versions, so it can't show what the console does with memory pools, executable memory
or firmware differences, and its timings mean nothing: there, the float and FMA kernels run tens
to hundreds of times slower than native code even on the same CPU. On the console, crash.log
decides.

Build (Ubuntu 24.04):

- GCC 14. Clang 21 and 23 can't compile its `std::ranges::to` against GCC 14.2's library.
- `-DFETCHCONTENT_SOURCE_DIR_FMT=<shadPS4>/externals/fmt` when GitHub archive downloads are
  blocked: spdlog would fetch fmt.

Run it headless from a folder with a `user/` subfolder (portable mode: the app's `/data` is
`user/data`):

```
SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 1920x1080x24" shadps4 <game>/eboot.bin
```

Here `<game>` is the PKG's staging folder without `sce_module/`: shadPS4 tries to run the
OpenOrbis stub modules in it and crashes. The first start may show a dialog about save
locations; answer "Do nothing" (xdotool can click it).

Version 01.01 ran to the end there: every CPU kernel in all three accuracy modes, three cores,
and all memory checks, with all checksums correct.

On the PS4 the benchmark runs on a 16 MiB-stack thread. Each recompiler core is created before
its guest memory, so the code caches get the executable pool first. Out-of-memory and xbyak
errors go on screen instead of crashing the app.
