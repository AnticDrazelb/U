# Eden for PS4: performance progress

Where the work stands, what was done, what was tried and ruled out, and what's next. Updated at
**04.05** (2026-11-02). Per-version detail is in [README.md](README.md) (one section a version,
newest first) and in the launcher's News page (`News()` in `src/platform_ps4.cpp`); the Eden changes
are the patches in [patches/](patches) (0001-0092, applied in order to Eden; see "How to work").

## Update at 04.85 (2026-11-07)

- **04.83's GPU split was slower on the console** (Breath of the Wild 6-10 fps with Two, 8-14 with
  One): shelved, kept behind the switch. Details in docs/gpu-thread-split.md.
- **04.84 measured each draw on the single GPU thread:** ~26.7 us of GPU thread time a draw outdoors,
  18.2 us of it the draw path. The biggest parts: uniforms 3.3, vertex/index 3.1, the hand-over to
  the Vulkan worker 3.0 (STATUS.md has the table).
- **04.85 hands over every 64 draws instead of 8.** Next: its console run, then the uniform and
  vertex/index parts (04.85 times the buffer cache's checks for the CPU's writes inside them).

## Update at 04.83 (2026-11-06)

- **The GPU thread split is built** (Graphics > GPU Threads: Two; One by default).
  - Patch 0173. Design and test findings: [docs/gpu-thread-split.md](docs/gpu-thread-split.md).
  - Its two open bugs from the 04.83 WIP: the crash at exit was Eden's nvhost-ctrl refusing a
    fence wait made while the last wait's signal was still finishing (patch 0174). libnx then
    didn't wait at all.
  - The hang never came back here, even without the fixes; its cause is unconfirmed.
- **The front end no longer stalls on the buffer cache's lock per macro call.** A lock-free bitmap
  of GPU-written granules answers whether a macro's parameters are GPU-written.
- **Next: the console test.** Breath of the Wild outdoors with Two against One: fps, and the 30 s
  line's back end busy % against the GPU thread's busy %. If the back end is now the limit, trim it
  (uniform copies, descriptors). If the front end is, look at SyncState's per-draw register
  compare (14 KiB) and the tasks' allocations.

## Update at 04.81 (2026-11-06)

What the console logs say after 04.73-04.81's CPU work (late in each session, 50% resolution,
Profiler on):

| Game | fps | 04.60-04.78 | Held back by |
|---|---|---|---|
| Captain Toad | 55-60 | 39-49 (04.40-04.60) | at the 60 cap most of the time |
| Mario Kart 8 | ~37 | 37-40 | the main thread (77% running) |
| Echoes of Wisdom | ~33 | ~33 | - |
| Silksong | ~30 | ~30 | the main thread (96% running) |
| Animal Crossing | 15-18 (GPU Accuracy Normal: no stalls) | 11-16, stalls on High | the main thread, GPU thread and GPU each partly idle |
| Breath of the Wild, outdoors | 8-10 | - | Eden's GPU thread (86% busy, ~21 us a draw, 4,300 draws a frame) |
| Little Kitty | ~8 | 7.5-8.5 | the main thread (90% running) |
| Bluey | ~9 | - | the main thread (93% running) |

- **04.80** (thread-local storage on fastmem, native copies in every game, native memcmp) and
  **04.81** (calls through PLT stubs into tiny functions inlined, GC loops one block) worked as
  designed (no page walks left, the stubs' callees inlined) but **moved no frame rate**. The
  sampler's per-core numbers show why: the hot code those targeted ran on the **worker threads**
  (Bluey's "current thread" calls on cores 1 and 2, its Unity job workers spinning on a lock; Little
  Kitty's GC loop 0.5% of core 0), not on the **main thread** (core 0) that sets the frame rate.
  Lesson: profiles are read per core from now on, the main thread's first.
- **The Unity games' main threads** (Little Kitty, Bluey, Silksong) spend 70-90% of their time in
  translated code spread over thousands of blocks (none above 0.6%), Eden's own code small (idle
  7%, clock_gettime 1%). Going further there means the translator's general code quality (the guest
  registers kept in host registers across blocks, ~21% of the host instructions are their loads and
  stores): a large change for an estimated 5-10%, not started.
- **Breath of the Wild outdoors** is held back by Eden's GPU thread, whose time has no hot spot either:
  [docs/gpu-thread-split.md](docs/gpu-thread-split.md): its work split over two cores (command
  processing on one, the rasterizer on another); 04.82 keeps every game's profile and measures the
  split, the split itself follows (04.83-04.84).
- **04.81's exit freeze fix**: four exits in the next session, none hung (the freeze was about one in
  ten).
- **Free**: in Breath of the Wild the PS4's GPU is 90% idle: resolution 100% costs no frame rate.

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
| 04.00 | 24-29 | Profiler on: no freeze in ~6 minutes |
| 04.01 | 26-30 | Profiler on (27-30 off): within a frame of 04.00 |
| 04.02 | no frame | translation 1.3 ms a block and rising (the look-ahead's invalidation ranges) |
| 04.03 | 26-30 | at **100%** resolution (28-29 most of the time), Profiler on: as 04.01 at 50% |
| 04.05 | 28-32 | the tester's **slowest track**, 50%, Profiler on: 30-s windows 29.5-31.5 (mean 30.5), 10-s 28.2-32.0 bar a race start dip; helpers' work a frame 20.7 -> 18.4 ms, the main thread now waits 6.2 ms a frame (8.3) and runs 78-80%: it alone is the limit; Eden's GPU thread 69% busy (50-66%); hot blocks: LEAs 12% -> 6% of instructions |
| 04.15 | 26-41 | Baby Park, Moo Moo Meadows (fresh boots): at equal draws a frame 32.8 -> 33.5 fps (800) and 26.5 -> 27.8 (1,150) against 04.12; pipeline key reused for 30% of lookups (mostly a duplicate lookup per draw), 0 mismatches in ~64,000 checks |
| 04.12 | 25-40 | Grand Prix, fresh boots: Baby Park (Crossing Cup 1) 42.8, 37.4, **25.3**, 30.7, then 36-38, 30.3-32.8 late; Moo Moo Meadows (Shell Cup 1) 31.0, 31.2, then 36.6-40.4. At the starts the main thread waits on `NVCTRL::NvEvent_0` (Eden's GPU thread reaching a syncpoint) up to 30% of its time; the PS4's GPU 7-11% busy. Uniform buffers: copy-every-draw mode in every frame, ~4 copied binds a draw (88-119k a second, 30-40 MiB/s); the game's constant data 0.2-0.28 MiB/s, half of it unchanged. A crash while loading: nvdrv's unmap recorded into the Vulkan scheduler (04.13 fixes) |
| 04.10 | 25-38 | three races from fresh boots (tracks to confirm; the bundle kept all five sessions): two on one track (2,750-2,870 images while loading) start 30.3/28.7 then 25.3/25.4 at 30-32k draws a second (~1,070-1,250 a frame), 31.4 after; the third 37.7, 34.0, 35.1 (~700 a frame). No crash, no render pass end skipped. The GPU thread ~72% busy at the third's start, ~29 us a draw (04.09: 28): the register writes' saving within the tracks' noise. At the starts the main thread runs 63-70% and waits on objects 10-19% (WaitSynchronization 17-21 a second, 5-9 ms each; 11 a second, 3 ms, later): something it waits for besides the GPU thread's work |
| 04.08 | 31-36 | the tester's **slowest track** (the Crossing Cup's first), 50%, Profiler on: 30-s windows 31.4-35.6 (mean 34.0), 10-s 30.7-36.7 after the start (mean 33.4), the start 32.2 then 27.6; GPU thread 75% busy at the start (81-85% in 04.07), ~23.8 ms a frame in the race (25.6); 26,000-28,000 draws and ~2.7M command words a second, 36% of the words taken in runs. The cup's other tracks 38.1, 39.6, 35.9 on average (17,000-26,000 draws a second), their starts without a dip: the first race's start translates the track's code for the first time (124 ms of the cores' time a second, later races 7-20). The previous session (04.08, 1,387 s in, after a race) logged no frame for 20 s: every host thread asleep, the game's threads in waits; asked the tester |
| 04.07 | 29-34 | the tester's **slowest track**, 50%, Profiler on: the tester's target (30 minimum, 32 average) reached but for the race start; second race 30-s windows 30.8-33.6 (mean 32.1), 10-s 29.4-34.4 after the start (25.7, 28.2 at the start); main thread runs 73-76% (77-80), waits on locks 2-3% (4-6) and now on objects 5-9% (0-1); scheduler lock found taken 5,500-9,000 times a second, 97% got by spinning (3 us), ~180 slept (75 us); main thread's ArbitrateLock ~20 us a call (35); Eden's GPU thread 77% busy (69): next to become a limit |
| 04.04 | 28-31 | the tester's **slowest track**, 50%, Profiler on: median 29.6 (04.01's usual track: 28.1); another track 30-33 (median 30.8); flag code gone from 93% of the hot blocks' samples |

The PS4's GPU is busy only 5-10% of the time in races, so resolution and GPU settings barely move
the frame rate: it's CPU-bound. Other games: Celeste plays smoothly; Captain Toad (64-bit) was at
about 15 fps in 03.80, limited by its main thread.

### What a race frame is made of (03.98-04.01 logs, Profiler on)

- **Threads:** the game's main thread (core 0, priority 44) runs ~75% of the time and waits ~24% on a
  condition variable for two helper threads (priority 44, core 2: runs ~62%, core 1: ~44%): a job
  system. A priority-32 thread on core 1 runs ~12% and waits on events. Core 3 (Eden's
  guest-core services, e.g. hid) is idle ~98%.
- **Core 0's time:** ~53% the game's translated code, ~8% Eden's own code (scheduler, SVC
  handling, spread thin), the rest idle (spinning in `PhysicalCore::Idle`, or asleep).
- **The translated code has no hot spot:** the busiest block on core 0 is 0.2% of its time.
  Speed has to come from translating everything better, or from less waiting between threads.
- **Block entries:** of the samples that land in translated code (04.01, core 0, races), a third
  land in a block's first 16 bytes (a quarter of the bytes past 64): getting into a block (its
  first fetch, the jump there, the first loads of guest registers) costs more than its length says.
- **Host instructions per guest instruction:** 5.85 in the 60 hottest blocks (04.01).
- **Tracks differ** (the tester: fps depends heavily on the map). A race frame's cost, from the
  `running` lines (main thread running / waiting for its helpers; the core 2 and core 1 helpers'
  running time): 04.01, usual track: 27.1 / 8.4 ms, 21.6, 15.7 ms (28.1 fps); 04.04, the slowest
  track: 25.3 / 8.4 ms, 20.8, 15.1 ms (29.6 fps); 04.04, another: 24.6 / 7.9 ms, 19.6, 14.8 ms
  (30.8 fps). The slowest track on 04.04 runs faster than the usual one on 04.01: 04.02-04.04
  took at least ~7% off the main thread's work. The helpers' ~8 ms the main thread waits each
  frame is on every track. Three tracks measured: all limited by the game's own CPU work the same way (main thread 73-77%
  running, 23-26% waiting on its helpers, which run ~61% and ~45%); the PS4's GPU 10-12% busy,
  Eden's GPU thread 50-66%; writes to GPU-cached memory up to 102,000 a second late in a race but
  under 0.5% of any game core in the profile, and no fps change with them. A slow track's log
  would show whether it is the same limit (more game work a frame) or another (Eden's GPU thread
  near 100%, the CPU waiting for the GPU).
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

- 04.01: an add or a shift by 1-3 whose operand is used again is a `lea` into a new register (it
  used to copy the operand out of the way first: every `[r3, #8]` with `r3` used again paid a
  `mov`); a conditional branch right after the flags are stored takes them from the register.
  11-19% fewer host instructions in the harness's loops, calls and structure accesses.
- 04.02: dead flags at a block's exit. A block ending in links reads the ARM code its successors
  start with (branches and calls followed; returns, indirect jumps, SVC count as reads); when they
  set the flags before reading them, the block doesn't store its flags, and its conditional branch
  is `cmp a, b; jcc` (or `test`) instead of `cmc/lahf/seto`, a store, a load and `sahf/cmc`.
  Mario Kart's hottest loops 760 to 536 instructions an iteration, game-like C 7% fewer.
  (04.02 registered the code read as one range per block, megabytes after a call: translation
  slowed to 1.3 ms a block and Mario Kart reached no frame; 04.03 registers each run read.)
- 04.05: constant offsets in fastmem accesses (`[r13 + base + 8]`, a per-access fault stub rebuilds
  the address), float compare branches fused (`ucomiss; ja`, the FPSCR's NZCV dead at a block's
  exit by the same look-ahead), no zero extensions after LDRB/LDRH or before STRB/STRH. 13% fewer
  instructions than 04.04 in Mario Kart's hottest loops and game-like C, 9% in zlib.
- 04.04: calls and returns end the look-ahead's paths with the flags set (the procedure call
  standard; 04.03's console run showed most hot blocks lead to a return or a virtual call first),
  and a block whose flags stay live branches on its compare instead of reloading them. zlib 20%
  fewer instructions, game-like C 8%.
- 04.07: each block's rarely run code in cold chunks apart from the blocks (L2 instruction misses
  -6% in a large game-like program, L1 unchanged); an inline return or fast dispatch takes the PC
  `BXWritePC` just computed from registers (game-like C -1.0% instructions).

### Threads and scheduling

- Pinned threads (setting, on by default): emulated cores 0-2 and the GPU thread each on a core of
  module 0; everything else on module 1 (03.79).
- Idle emulated cores spin before sleeping (50 us, 03.78; 1 ms on pinned cores 0-2, 03.97); the
  services' host threads spin before sleeping too (03.78) and run at VeryHigh priority (03.93).
- Code caches 48/40/32/8 MiB for cores 0-3 (128 MiB of system flexible memory in all, 03.95); from 04.21 as large as the pool allows (tiers up to 160/96/96/8).
- Eden's host locks (the kernel's scheduler and timer locks, the cores' context locks, CoreTiming's
  queue) spin up to 20 us before sleeping on the PS4; the timing thread is woken only for a new
  earliest event (04.06). Per-lock counts in the 30-second line.

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

- **Translator changes tested on small programs only:** 04.02 passed 3,200 random programs and
  sped up the benchmarks, but its invalidation ranges grew with the number of blocks; at a game's
  scale translation slowed 10x and more. `tools/dynarmic-harness/manyblocks.py` (thousands of
  functions, calls across a megabyte) times translation at scale: run it against the old build for
  any change to what a block records.

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
- **Writes to GPU-cached memory through the callbacks** (tens of thousands a second in races):
  invisible in 04.00's profile, so cheap; not worth chasing for speed.
- **Following unconditional branches into one block (superblocks):** 90% of the hot blocks end in
  a conditional branch (loops), under 3% in a plain `b`: little to gain.
- **alechurri/nx-on-orbis** was looked at: nothing it had that this port didn't (owner's call:
  "ours is far superior").

## Small savings on the main thread's path (a running list)

The tester's point (04.06): savings of 1-2% each are worth having when there are several, and at
30.5 fps each 1% of the main thread's time is ~0.33 ms a frame (~0.3 fps). The test is whether a
saving is on the main thread's path (core 0), not its size. Kept here until done or ruled out.

| Item | Share (estimate) | State |
|---|---|---|
| Inline return/fast dispatch reloads the PC it just stored | -1.0% instructions (game-like C) | done 04.07 |
| `__tls_init` on every `GetCurrentEmuThread` | 0.18% + 0.16% of core 0 | done 04.07 |
| `weak_ptr::lock` per queued event in `UnscheduleEvent` | part of 0.13% | done 04.07 |
| Rarely run code between the hot code (I-cache, L2) | L2 instruction misses -6% (sim) | done 04.07 |
| Service requests: hand-off latency (~80 us before taken, ~250 us a main-thread call) | up to ~2% | 04.07: spin to 250 us after short waits (likely few); its log line says how they wake; next: quick requests on the calling thread |
| Host lock sleeps (`_umtx_op` 8.3% of core 0) | up to ~9% | 04.06: spin before sleeping; check its log line |
| memcpy on core 0 walks the page table for loads and stores (declined at boot, "pages not back to back") | ~0.2-0.3% of core 0 | open: retry declined accesses, or an inline marked-page check with fastmem |
| Flags across chains of conditional instructions, `ConditionalSelect` reloading NZCV | 1-2%? | open, needs measuring |
| `UpdateHighestPriorityThreadsImpl` | 0.33% of core 0 | Horizon's algorithm; nothing obvious |
| `SetSvcArguments` | 0.22% of core 0 | eight copies: probably sampling skid after the SVC |
| Guest register file in memory (cross-block register caching) | ~25% of instructions | open: the biggest and hardest |
| **GPU thread** (04.07: the race start's limit, 81-85% busy; 77% later) | | |
| Command stream: a call per register write (`DmaPusher::CallMethod`) | part of 15% of the GPU thread | 04.08: runs in one loop; 04.10: the 3D engine's runs straight into its registers (`SinkRun` + `ConsumeSinkImpl` were 4.6% at a start) |
| `UpdateVertexBuffer` called for all 32 slots per draw | ~2% of the GPU thread | 04.08: dirty ones only |
| `VertexFormat` / `IsFormatSupported` per attribute per draw | ~0.7% | 04.08: cached |
| `IsGPUFenceBehaviorAccurate` per `FindBuffer` | 0.23% | 04.08: once a frame |
| Samplers (`GetSamplerId`, `Sampler::HandleFor`, `MakeKey`) | ~1.4% | open |
| `TextureCache::SynchronizeAliases` at a race's start | 2.7% (0.3% mid-race) | open |
| `FixedPipelineState::Refresh` every draw (+ `Next`'s key compare) | 1.3-2.1% (+0.7%) | 04.15: skipped when none of its 412 registers (found by tools/gpu-tests/pkeyregs.sh) changed; every 64th checked; the 30-second line gives the share reused |
| Uniform buffers copied every draw (Eden's skip-cache path), identical constant buffer rewrites invalidated | memcpy 3-5% + stream buffer work | 04.12's counts: ~4 copied a draw, 30-40 MiB/s; the game's constant data 0.2-0.28 MiB/s, half identical (too little to matter). The cached path would end render passes for uploads; reusing a copy needs a content compare (about the copy's cost): no clear gain yet |
| `GpuToCpuAddress` | ~2% | 04.16: texture descriptors read in place from the pool's host range, handles with one walk per page (Ryujinx comparison, 4) |
| A second pipeline lookup per draw (`UpdateDynamicStates`) | ~half of 04.15's lookups | 04.16: the draw's pipeline passed |
| Eden for PS4's own counters on the GPU thread (locked adds, cache lines shared with other threads' counters) | ~1.1% (04.12's two per uniform buffer bind) | 04.14: own cache line, unlocked adds |
| Macro parameters copied into `macro_params` (`vector::insert`) | 0.4% | 04.14: read in place when they come in one call |
| Buffer cache bindings, memory tracking (`WordManager`) | ~9% + ~7% | open |
| **Stability** | | |
| Exit to Eden's menu froze once (04.08, after 23 min: `ShutdownMainProcess` never returned) | | 04.09: the step in the hang report; restart after 30 s |
| **First race's start** (27.6 once in 04.08; later races' starts don't dip) | | 04.09's counters (a third track, fresh boot): not new work. The loading's 10 s before the race make 117 pipelines, 3,059 images, 452 buffers; the race's first 30 s 1-3 pipelines, 46-192 images a 10 s, no draw skipped. The start has twice the draws: ~780-880 a frame (32-34 fps), later 360-410 (40-42). The GPU thread 77-81% busy there, ~28 us a draw; the main thread waits on objects 13%. So: Eden's cost per draw (headroom, the same work) |
| What the main thread waits on at a race's start (objects 10-19%, 5-9 ms a wait) | | 04.11: the waits by what ended them, per core |
| First race of a session slower than the rest (any track; back after a relaunch) | | Not pipelines (0-2 new per 10 s, the shader cache rebuilds them at boot); the CPU code translated the first time (134-173 ms/s vs 22): 04.17's persistent translation cache |
| 64-bit games translating as they go (Animal Crossing up to 683 ms/s, Echoes of Wisdom 75-285 in play) | | 04.18: the translation cache for A64 too |
| The translation cache never used (04.17/04.18: 0 of 163,801 blocks in Mario Kart, 0 of 60,902 in Zelda) | | 04.19: the code region was at a random offset every boot; now fixed per title. Relocations only at 64-bit immediates; a cache whose run didn't end normally is deleted |
| 64-bit games without A32's translator savings (fold offsets, redundant extensions, cold code) | | 04.22: ported to A64 (1,500 programs the same as before; -0.9% instructions, -6% I1 misses on game.c for AArch64) |
| Bluey: a third of the hottest code's accesses walk the page table (fastmem declined once, "pages not back to back") | | 04.22: counted per core in the 30-second line; next |
| 64-bit games' flags (11% of the translated code's instructions on game.c for AArch64), floats through general registers, branches through a stored bit | | 04.23: branches on the host's flags, dead flags with a look-ahead (calls and returns end it), floats in vector registers: -21.0% instructions vs 04.22 |
| Echoes of Wisdom: 50-84 thousand writes a second to memory the GPU caches leave the inline walk | | 04.23: counted by what they cost (same page, collected, invalidating); next |
| 64-bit selects after compares through the stored flags; returns putting the location descriptor together (7 of 14 instructions) | | 04.24: `cmp; cmov` in place, the return stack of return addresses (emptied on FPCR mode changes): -27.3% instructions vs 04.22 with Eden's options |
| 64-bit accesses through one base grouped into a page walk even with fastmem (30 instructions for 4 loads instead of 12); load-acquires as locked exchange-adds; DMB ISHLD/ISHST as full barriers | | 04.25: shared walks for fastmem's declined accesses only (-8.4%, -33.4% vs 04.22), load-acquires plain, LD/ST-only barriers elided |
| 64-bit float loads through general registers; sign-extending loads in two instructions | | 04.26: `movd`/`movq xmm, [mem]`, `movsx`/`movsxd` from memory, SXTW of W reads from the JIT state: -36.3% vs 04.22. Superblocks tried, dropped (+13%) |
| 64-bit float constants in two instructions; return stack of 8 with an and-mask on every call and return | | 04.27: one load; 256 entries, a byte pointer: -38.0% vs 04.22 |
| Unity (IL2CPP) code cut into 2-3 instruction blocks by null checks; float stores through general registers | | 04.28: forward CBZ/TBZ as side exits, stores from XMMs (unity.c: -22% blocks entered, -3.0% instructions) |
| 04.23-04.27: the flags look-ahead read B.cond/CBZ/TBZ targets 1-3 bytes off (rarely took live flags for dead) | | 04.28: fixed (found by the full sweep, gen8) |
| Every B.cond ending a block (63% of game.c's block ends) | | 04.29: forward ones as side exits, flags stored only where the target reads them: -7.4%, -43.2% vs 04.22 |
| Boot crash at 6-8 s (Captain Toad, Little Kitty): a stick's movement reached NPad::ControllerUpdate before the HID resource existed | | 04.34: returns until it does (patch 0141) |
| Captain Toad level 3: the app ends while loading, no crash report (GPU log stops mid-run, no teardown) | | open: a system-side kill (a GPU fault?); the PS4's error code, if it shows one, tells |
| The heap's overflow took 270 MiB of the ~364 MiB of flexible memory in a game's first seconds | | 04.33: direct memory first, flexible only when that fails |
| Animal Crossing: "thread constructor failed" at 330 s (16 MiB default stacks; the next one couldn't be had) | | 04.32: 4 MiB for threads asking for no size, a retry at 2 MiB (untested on the console yet) |
| Bluey: boot crash in 04.30 at 64 s (a page table's page unmappable: flexible memory out; 1 MiB free at the end of 04.21's run) | | 04.31: such pages from direct memory once flexible memory is out (untested on the console yet) |
| FPS counter frozen through stalls, showing the rate from before them | | 04.31: red for 2 s after a frame 100 ms or more late; the rate taken at once after 250 ms |
| SBFM/UBFM (LSL, LSR, UBFX, SBFIZ) as a rotate and two masks (2-9 instructions); every W result zero-extended again before its store (3.3% of game.c's instructions) | | 04.30: shifts (1-2); W values zero above 32 bits stored and widened as they are, LeastSignificantWord a no-op for 32-bit uses: -4.7%, -45.8% vs 04.22; unity.c -3.6% |
| Code caches too small outside Mario Kart (Silksong core 0: 8 whole flushes in 270 s, 250-550 ms/s translating; Zelda core 2: 5 in 6 min) | | 04.21: sized from the system pool's free memory in fixed tiers (up to 160/96/96/8 MiB), the app heap's share of the pool 384 -> 192 MiB |
| Settings lists ran off the screen (Graphics) | | 04.21: scrolled with the selection, description kept on screen |
| Zelda: aborted at 45-56 s (std::bad_alloc), two runs in a row | | 04.20: dynarmic's link table, 57 MB in one piece per core (232-byte flat map slots), now 48-byte slots; a new overflow heap serves its request before other threads see it |
| Zelda: the GPU stops at the same scene load 5.5-6.5 min in (04.18, 04.19) | | 04.20: Draw Shortcuts setting (04.15/04.16's savings off) to test whether they are the cause |
| Zelda: 0.6 fps after a run in which the GPU hung | | 04.19: not the cache (it loaded none in the first slow run); the GPU started Eden's work 0.25-2 s late, busy 0-2%. Flagged in the 30-second line; to test: a full PS4 restart, then Shader Cache off |
| The shader cache file deleted whole when an append was cut short (crash, power cut) | | 04.17: cut back to the last whole entry; `vulkan_pipelines.bin` written then renamed |
| RADV crash while a game loads (04.09, once): `end_subpass` (Mesa's common render pass code) on the Vulkan worker, a NULL + 0x60 read: a render pass ended with none begun | | 04.10: skipped and counted on the worker; the cause found in 04.12's crash (nvdrv's unmap realizing a deferred clear into the scheduler): 04.13 |
| The log bundle keeps the last two sessions only: the tester's four boots (three tracks) left the last two | | 04.10: five |

## Next (most promising first)

1. **Same track, two versions** (04.01 vs 04.04, 50%, Profiler on) to measure 04.02-04.04's gain
   (04.04's console run was on another track); and the **slowest track's** log; race fps (same track as before, Profiler on or off); a
   `hotblocks.log` to see the flag code gone.
2. **Translated code, generic** (where the main thread's time goes): flags still stored and reloaded
   across chains of conditional instructions (`ldrbeq; cmpeq; beq`: three blocks); `ConditionalSelect`
   still reloads NZCV (a conditional MOV after a compare in the same block); flags still stored where a
   successor returns or jumps through a register first (a conditional early return); the inline
   RSB pop reloaded the PC/upper descriptor it just stored (done 04.07); the guest register file in memory (27%
   of hot instructions) needs cross-block register caching: the biggest and hardest.
3. **Block entry costs** (a third of the translated code's samples): the deferred emits are apart
   since 04.07 (cold chunks); the exits' own slow paths (a link's halt fallback with its RSB push,
   ~70 bytes) still sit in the hot code. 04.05's busiest blocks (memset, memcpy) run on cores 1
   and 2: core 0's code is spread thin, no block above 0.25%.
4. **Writes to GPU-cached memory** climb through a race (tens of thousands a second) but cost
   little (04.00 profile); only if the GPU thread (65% busy) becomes the limit.
5. **Contended mutex cost** in Eden's kernel (`ArbitrateLock`/`ArbitrateUnlock`, ~11 us an
   unlock; core 0 8.3% in `_umtx_op` in 04.05): 04.06 spins before sleeping on the host locks;
   its log line says which locks are found taken and how often. Next if it stays: the services'
   condition variables (`SendSyncRequest` ~255 us), `UpdateHighestPriorityThreads`,
   `RescheduleCores`.
6. **TLS pages** (`KThreadLocalPage`, one 4 KiB page from the `KPageBuffer` slab at a random
   address): never lined up, so thread-local reads walk. Allocate them in owned, lined-up 16 KiB
   groups.
7. **Core 3's services** (hid): ~100 us before a call is taken; core 3 shares module 1.
8. **Core 0's code cache** fills in ~6 minutes; smaller code (e.g. the halt fallback of backward
   links) or a different split.

## How to work

- **A fresh container:** `tools/fresh-container.sh` sets everything below up (clang 21, Eden at
  `eb88a40` with the patches, its dependencies, the PS4 SDK, libc++, the sysroot's libraries with
  OpenSSL, Mesa, FFmpeg, libnx), about 45 minutes; the first `build.sh ps4` takes about 25 more.
  The import check against Crema's needs `CREMA_ELF` (a Crema build's ELF), which a fresh container
  doesn't have.
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
  differential tests of translator changes (old build vs new on random programs). Build the old
  one first (`git stash` in `~/eden`, `./build.sh dynbench_base`, `git stash pop`). No hardware
  counters in a VM: count instructions with `valgrind --tool=lackey --basic-counts=yes
  --smc-check=all` at two iteration counts and take the difference.
- **Reading a console log:** fps from `speed:` lines; the `running N s:` lines for everything else
  (look at race windows, not loading); with the Profiler, `samples.log` windows by thread and
  `hotblocks.log`. Compare like with like: same track, same Profiler setting.
