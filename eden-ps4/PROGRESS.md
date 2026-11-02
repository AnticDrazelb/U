# Eden for PS4: performance progress

Where the work stands, what was done, what was tried and ruled out, and what's next. Updated at
**04.00** (2026-11-02). Per-version detail is in [README.md](README.md) (one section a version,
newest first) and in the launcher's News page (`News()` in `src/platform_ps4.cpp`); the Eden changes
are the patches in [patches/](patches) (0001-0087, applied in order to Eden; see "How to work").

## Goal

Mario Kart 8 Deluxe (32-bit ARM, the main test game) racing at 25-30 fps on a base PS4, ideally
33-35 so the dips stay above 25, with as few stutters as possible. The PS4 gives the app 7 Jaguar
cores at 1.6 GHz (two modules of four, the seventh core shared with the system).

## Where it stands

### Mario Kart 8 Deluxe race frame rate by version

| Version | Races | Settings / notes |
|---|---|---|
| before 03.78 | 12 fps | menus 50-60 |
| 03.79 | 15-18 | one walk per LDM/STM, pinned threads |
| 03.84-03.86 | 14-17 | fastmem probe, then fastmem on |
| 03.87 | 16-19 | exclusives inline with fastmem |
| 03.91 | 21 | leaner translated code (03.89/03.90), crash fixed |
| 03.92 | 19-20 | 100% resolution, Profiler on |
| 03.93 | 22-24 | 75%, Profiler on: declined accesses share a walk |
| 03.94 | 20-23 | 50%: walking every stack access cost ~10% (undone in 03.95) |
| 03.96 | 25-29 | 50%: stacks lined up for fastmem (03.95) |
| 03.97 | 24-28 | NEON loads/stores faster; no visible change (different track) |
| 03.98 | 23-26 | Profiler on (costs a little) |
| 03.99 | 25-29 | Profiler on: the game's globals on fastmem; one 20 s freeze |
| 04.00 | not tested yet | the freeze's cause fixed |

The PS4's GPU is busy only 5-10% of the time in races, so resolution and GPU settings barely move
the frame rate: it's CPU-bound. Other games: Celeste plays smoothly; Captain Toad (64-bit) was at
about 15 fps in 03.80, limited by its main thread.

### What a race frame is made of (03.98-03.99 logs, Profiler on)

- **Threads:** the game's main thread (core 0, priority 44) runs ~75% of the time and waits ~24% on a
  condition variable for two helper threads (priority 44, core 2: runs ~62%, core 1: ~44%): a job
  system. A priority-32 thread on core 1 runs ~12% and waits on events. Core 3 (Eden's
  guest-core services, e.g. hid) is idle ~98%.
- **Core 0's time:** ~53% the game's translated code, ~8% Eden's own code (scheduler, SVC
  handling, spread thin), the rest idle (spinning in `PhysicalCore::Idle`, or asleep).
- **The translated code has no hot spot:** the busiest block on core 0 is 0.2% of its time.
  Speed has to come from translating everything better, or from less waiting between threads.
- **Instruction mix of the hottest translated code:** ~23% loads/stores of the guest register file
  (in `JitState` memory), ~10% flag handling (`cmc`/`lahf`/`seto` to save NZCV, `sahf` to read it),
  ~8% guest memory accesses (fastmem, one `mov` each), the rest the work itself.
- **Synchronisation:** core 0's threads make ~1,100-1,300 contended `ArbitrateLock` calls a second
  (~38 us each) and ~2,100 `ArbitrateUnlock` (~12 us each): a mutex the main thread and the core-2
  helper take in turns. ~5-8% of the main thread.
- **Services:** ~560-600 calls a second, ~165 us each round trip (~80 us before the service's
  thread takes it). The main thread spends ~2% in service calls. hid calls wait ~100 us for
  core 3.
- **Writes to memory the GPU caches** go through the slow callbacks: they climb during a race from
  ~1,000 to ~65,000 a second (mostly core 1's memcpy into GPU buffers). Open.
- **Fastmem declines** ("pages not back to back") are now ~0 after boot (were 2,000 a second at
  boot and 150-600 a second later before 03.99).
- **Code caches:** core 0's 48 MiB fills after ~6 minutes and is flushed whole, so far always during
  a loading screen (harmless there).

## Done: what worked

### Guest memory access

- **Page table walk shared** by the accesses of one instruction or base (`A32SharedWalkPass`,
  `A64SharedWalkPass`, `*HostPointer` + `*MemoryVia*`): PUSH/POP of 9 registers was 9 walks
  (03.79, A64 in 03.81).
- **Exclusives inline** (LDREX/STREX through the page table, no monitor callbacks), cheaper DMB
  (03.80); kept inline when fastmem declines them (03.87).
- **Fastmem on the PS4** (03.84-03.86): `sceKernelEnableDmemAliasing` allows the same direct
  memory at two addresses; a 2^39 arena at 0x4800000000 maps the Switch's memory at its own
  addresses lazily (a fault maps a 16 KiB page from the page table). Needs
  `cpuopt_unsafe_host_mmu` (the launcher sets it). Pages the GPU caches are mapped read-only.
- **The Switch's RAM on demand** (03.70): direct memory taken in 2 MiB chunks as touched.
- **16 KiB host pages vs 4 KiB guest pages.** A 16 KiB arena page can only be mapped if its four
  guest pages are consecutive in the DRAM and at their places in a 16 KiB page. Three causes found
  and fixed:
  - thread stacks (game-aliased with `svcMapMemory` at 4 KiB steps): the kernel puts the alias on a
    lined-up copy with spare pages around it (arena padding), copied back on unmap; copies from the
    applet pool, at most 128 MiB (03.95, `k_page_table_base.cpp`, `orbis_memory.cpp`);
  - the main thread's stack: 16 KiB-aligned, one piece (03.95);
  - **the code region**: Eden's loaders put it at 0x200000 plus a random 4 KiB step, so three boots
    in four every 16 KiB of the game's code, constants and globals was out of line; the step is
    16 KiB on the PS4 now, and an allocation's last <64 KiB comes as one aligned run (03.99).
  Still out of line: thread-local storage pages (see Next).
- **After a fastmem decline**, the instruction is recompiled to walk; stack/TLS accesses of a core
  walk from the start once 32 stack (or one TLS) declines happened there (03.94/03.95).
- **NEON VLD1/VST1** a whole register at a time (one 64-bit access per D register, Q pairs kept as
  one value) instead of an access per element: a copy loop 6x faster on the harness (03.97).

### Translated code (dynarmic x64)

- 03.89: `B<cond>` ends the block with an `If` terminal; no per-block `rbp` frame; forward links
  without a halt check; register allocator moves values to free registers instead of spilling;
  inline return-stack-buffer pop.
- 03.90: predicated ARM instructions translated in place (`ConditionalSelect`) instead of
  splitting the block; `VMRS APSR_nzcv` after `VCMP` forwarded; inline fast dispatch for indirect
  jumps.
- 03.91: one xbyak configuration for the whole program (the strict copy from `video_core` broke
  `jmp qword[...]`: Mario Kart crashed at start in 03.89/03.90).

### Threads and scheduling

- Pinned threads (setting, on by default): emulated cores 0-2 and the GPU thread each on a core of
  module 0; everything else on module 1 (03.79).
- Idle emulated cores spin before sleeping (50 us, 03.78; 1 ms on pinned cores 0-2, 03.97); the
  services' host threads spin before sleeping too (03.78) and run at VeryHigh priority (03.93).
- Code caches 48/40/32/8 MiB for cores 0-3 (128 MiB of system flexible memory in all, 03.95).

### Stability

- A fresh process for every game (03.88): the C heap kept what the last game took.
- 04.00: a 20 s freeze (03.99 log): the GPU driver's and audio's service threads spun on a
  statistics lock whose lower-priority holder couldn't get a core. The statistics locks are
  mutexes now; probably the earlier Captain Toad "no new frame for 20 s" too.

### Diagnostics (all in the log bundle; the web page or a USB drive gets it)

- `boot-trace-launcher.log`, every 10 s: fps and ms of work a frame; every 30 s a long line (split
  into "(continued)" pieces since 03.98): idle wakes, service call round trip split
  (queue/handling/resume), timer events, translations and flushes, slow memory accesses, code
  cache free per core, core idle %, vsync/buffer waits, **each core's system calls by time**,
  **the busiest guest threads (running %, waits by kind)**, kernel timer events, fastmem faults
  and declines by reason, stacks lined up, the five busiest service commands (with each one's
  queue wait; nvdrv ioctls listed individually).
- Profiler (System Settings > CPU): `samples.log` (1000 samples a second per thread, per 10 s
  window) and `hotblocks.log` (the hottest translated blocks: guest words and host bytes, to
  disassemble with llvm-mc). The log bundle's summary names Eden functions and guest blocks.
- `hang.log` (no frame for 20 s: every thread's stack), `crash.log`.

## Tried and ruled out (or not worth it now)

- **GPU-bound?** No: the PS4's GPU is busy 2-10% in races; resolution 50/75/100% and Frame Pacing
  Unlocked (03.84) changed nothing measurable.
- **Walking every stack access from the start** (03.94): fewer declines, but ~10% slower than
  fastmem; replaced by lining stacks up (03.95), walks only after 32 declines.
- **Longer idle spins** (03.97, 1 ms): sleeps down from ~1,800 to ~400 a second, no visible fps
  change; kept (free on dedicated cores).
- **NEON load/store speed-up alone** (03.97): real (audio mixer, vector maths) but not on the main
  thread's critical path; no visible fps change by itself.
- **Timers:** events on time (2-3 us late); ~8,500 a second, mostly Eden's own periodic work (HID
  at 1 kHz, audio, motion, preemption) on module 1, not a bottleneck seen.
- **Service calls:** ~2% of the main thread; not worth a redesign yet.
- **Clock reads:** already from the TSC (`__wrap_clock_gettime`); 04.00 only removed a shared
  counter its every call incremented.
- **alechurri/nx-on-orbis** was looked at: nothing it had that this port didn't (owner's call:
  "ours is far superior").

## Next (most promising first)

1. **Test 04.00** on a console: no freezes; race fps (same track as before, Profiler on or off).
2. **Writes to GPU-cached memory** (1,000 to 65,000 a second, climbing through a race): find why
   these pages stay tracked (Eden's buffer cache `OnCPUWrite` returning "GPU modified", so every
   write is collected through the callback). Probably core 1's memcpy into a streamed buffer.
3. **Translated code, generic:** the terminal `If` and `ConditionalSelect` reload NZCV from memory
   right after storing it (use the register); the inline RSB pop reloads PC/upper descriptor it
   just stored; flag materialisation (`cmc`/`lahf`/`seto`) when nothing reads V or C; the guest
   register file in memory (23% of hot instructions) needs cross-block register caching or
   superblocks: the biggest and hardest.
4. **Contended mutex cost** in Eden's kernel (`ArbitrateLock`/`ArbitrateUnlock`, ~12 us an
   unlock): profile `KScopedSchedulerLock`, `UpdateHighestPriorityThreads`, `RescheduleCores`,
   `PhysicalCore::Interrupt`.
5. **TLS pages** (`KThreadLocalPage`, one 4 KiB page from the `KPageBuffer` slab at a random
   address): never lined up, so thread-local reads walk. Allocate them in owned, lined-up 16 KiB
   groups.
6. **Core 3's services** (hid): ~100 us before a call is taken; core 3 shares module 1.
7. **Core 0's code cache** fills in ~6 minutes; smaller code (e.g. the halt fallback of backward
   links) or a different split.

## How to work

- **Branch:** `eden-ps4-stage2` (this repo). Each version: Eden commit(s), exported as the next
  patch(es), `VERSION`, a README section and a News entry, then the package.
- **Eden tree:** `~/eden` with `patches/` applied in order as commits (`build.sh` header names the
  base). A new patch: commit in `~/eden`, then
  `git format-patch -1 --start-number <N> -o eden-ps4/patches`.
- **PS4 package:** `CMAKE=<cmake 3.31> ./build.sh ps4` (from `eden-ps4/`): `out/EdenPS4.pkg`; each
  build's ELF is kept in `~/eden-ps4-elves/` for symbolising log addresses (`llvm-symbolizer` /
  `llvm-objdump` against the matching version).
- **Desktop:** `./build.sh host`, or `tools/desktop-test-build.sh` (also turns on the kernel line-up
  code with `EDEN_TEST_ARENA_LINE_UP`); then `./test.sh` (hello, games, `game/stacktest.c`...).
  `./ui-preview.sh` renders the launcher's screens to PNGs.
- **dynarmic harness:** [tools/dynarmic-harness](tools/dynarmic-harness): benchmarks and
  differential tests of translator changes (old build vs new on random programs).
- **Reading a console log:** fps from `speed:` lines; the `running N s:` lines for everything else
  (look at race windows, not loading); with the Profiler, `samples.log` windows by thread and
  `hotblocks.log`. Compare like with like: same track, same Profiler setting.
