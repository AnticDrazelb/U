# Proposal: the GPU thread's work on two cores (04.82 onwards)

Status: **approved, not started** (2026-11-06, after 04.81). Goes with the profiler keeping every
game's profile (below), which ships first.

## Why

The games split into two groups by what holds them back (04.80/04.81 logs, per-core sampler data):

| Game | fps (late in a session) | Held back by |
|---|---|---|
| Captain Toad | 55-60 | nothing much: at the 60 fps cap most of the time |
| Mario Kart 8 | ~37 | the main thread (77% running) |
| Echoes of Wisdom | ~33 | - |
| Silksong | ~30 | the main thread (96% running), translated code spread over thousands of blocks |
| Animal Crossing | 15-18 | partly everything: the main thread, Eden's GPU thread and the PS4's GPU each idle part of the time |
| **Breath of the Wild, outdoors** | **8-10** | **Eden's GPU thread** |
| Little Kitty | ~8 | the main thread (90% running), flat profile |
| Bluey | ~9 | the main thread (93% running), flat profile |

Breath of the Wild outdoors (04.81, a 480 s session, Profiler on, 50% resolution):

- 40,000 draws a second, about 4,300 a frame;
- Eden's GPU thread busy about 86% of the time (79% in Eden's code, 14% in system calls);
- the game's main thread waiting on objects (for the GPU thread's work) 45% of the time;
- the emulated cores about half idle; the PS4's GPU busy 10%;
- so about 21 us of the GPU thread's time a draw, ~90 ms a frame: the frame rate.

The GPU thread's time has no hot spot (the biggest function under 3%). By part, from the sampler's
named functions (56% of its samples; the rest spread too thinly to name):

| Part | Share of the GPU thread | Where it would run |
|---|---|---|
| Command processing: DmaPusher, the engines' methods, macros | ~13% | front end |
| GPU memory reads (command lists, constant buffers, descriptors) | ~8% | both |
| Textures, samplers, image descriptors | ~8% | back end |
| Buffer cache, uniform buffer copies (5 a draw, 63 MB/s) | ~7% | back end |
| Pipelines, dynamic state, recording for the Vulkan worker | ~4% | back end |
| libc, allocation, standard library | ~4% | both |
| Not named | ~13% | ? |

Meanwhile the Vulkan worker thread, which replays what the GPU thread records, is 85% idle: it only
makes the Vulkan calls (cheap in RADV), all the deciding is on the GPU thread.

Shaving the parts one at a time gets 1-3% each, measured only on the console (the desktop test
build has no Vulkan renderer). Two threads sharing the GPU thread's work is the change that can
move Breath of the Wild by a noticeable step.

## What

Split the GPU thread into a **front end** and a **back end**, each on its own core, connected by a
queue:

- **Front end** (the GPU thread as now, PS4 core 3): fetches and decodes the command lists
  (DmaPusher), runs the engines' methods and the macros, keeps the register state the macros and
  methods need. Instead of drawing, it appends to the queue: register writes (as runs, the way
  DmaPusher already sees them), and markers - a draw (with any inline index or vertex data copied),
  a clear, a blit, a compute dispatch, a query, a syncpoint increment or semaphore release, a DMA
  copy, an inline-to-memory write.
- **Back end** (new, on one of PS4 cores 4-6, beside the Vulkan worker): keeps its own copy of the
  registers and their dirty flags, applies the writes in order, and runs each marker as the GPU thread
  does now: the rasterizer, the buffer and texture caches, pipelines, descriptors, uniform copies,
  recording for the Vulkan worker.

Everything that orders the GPU's work against the game goes through the back end, in order: syncpoint
increments and semaphore releases (so the game never sees its constant buffers consumed before they
were copied), the emulated CPU's cache flushes and invalidations (today queued to the GPU thread),
fences, framebuffer composition. The front end may run ahead only through pure command processing.

Where it must wait for the back end (the cases are rare and are found by measuring):

- a method or macro that needs a result only the back end has (a query result read back, a semaphore
  acquire on a value the back end writes);
- the queue full (a bounded queue, so the front end can't run frames ahead);
- shutdown, a flush the emulated CPU waits for, a framebuffer the display needs.

A menu switch (Graphics > GPU Threads: One / Two) and the boot watcher's safer step fall back to
today's single thread, so a game that misbehaves runs as before.

## Expected gain, honestly

If the split is ~30% front end, ~70% back end, the back end becomes the limit at ~70% of today's
GPU-thread work: at best ~1.4x for a game held back by the GPU thread, less the queue's costs.
**Breath of the Wild outdoors: 9 fps towards 11-13.** Animal Crossing a little. **No gain** for the
games held back by their main thread (Little Kitty, Bluey, Silksong, Mario Kart): their limit is
the translated code, not the GPU thread.

## Steps, each a release with a measurement

1. **04.82: every game's profile, and the measurement** (quick; ships while the split is built).
   - The profiler keeps every game's profile: `samples.log` and `hotblocks.log` kept as
     `-previous`, `-previous-2`... like the other logs, so a session of several games gives every
     game's profile, not only the last one's.
   - The GPU thread times its own two halves (command processing against the rest) and logs the
     split each 30 s, so the 30/70 estimate is measured on the console, not inferred from the
     sampler - it tells what the split can gain before it's switched on.
2. **04.83: the back end, behind the switch (off).** The register-write log and the markers, the
   back-end thread, the single thread unchanged when off. Checked on the desktop with the null
   renderer (it goes through the same front end, engines and rasterizer interface): frames and memory
   identical with one and two threads in the test programs (`test.sh`), plus the GPU tests.
3. **04.84: on for testing.** Breath of the Wild outdoors, Animal Crossing, Mario Kart, Captain Toad
   with the switch on and off; logs as usual. On by default only when it's faster and nothing breaks.

## Risks

- **Ordering bugs**: a marker run out of order shows up as flicker, missing or stale geometry, a
  hang. Mitigation: everything guest-visible through the one in-order queue; the desktop comparison;
  the switch and the boot watcher.
- **Shutdown and hangs**: one more thread to stop (04.81's exit fix shows how these go wrong): a
  timed stop, the hang watchdog's stacks for the new thread.
- **Memory**: the queue and the second register copy are small (tens of KiB to a few MiB), the
  register-write log ~20 MB/s through a ring.
- **Less gain than hoped**: if 04.82's measurement shows the front end well under 30%, the split's
  ceiling is lower than estimated; it stays behind the switch, and the back end's biggest parts are
  trimmed too.
