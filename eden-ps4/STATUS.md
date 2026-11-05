# Eden for PS4: status (after 04.56)

Install **04.56** (newest). Packages are built to `out/EdenPS4-<version>.pkg`. They are not in git.
The patches are in `eden-ps4/patches` (up to 0144). The Eden tree is `~/eden`, branch `ps4`. The
harness worktree is `~/eden-pptc`, branch `pptc64`; it is behind `ps4` from 0139 on.

## Done since 04.22

### CPU translator speed-ups, 64-bit games
Test program `game.c`: 3,335,063 host instructions in 04.22, 1,806,170 now (-45.8%).

- **04.23:** branches on host flags, dead flags elided, floats kept in XMM registers, constant
  addresses.
- **04.24:** selects on host flags, PC-only return stack.
- **04.25:** shared page walks only for declined accesses, plain LDAR, DMB LD/ST elided.
- **04.26:** float and sign-extending loads in one instruction.
- **04.27:** constants in one load, a 256-entry return stack.
- **04.28:** CBZ/TBZ side exits. Also fixes the 04.23-04.27 look-ahead target bug.
- **04.29:** B.cond side exits.
- **04.30:** bitfield instructions as shifts, no redundant W zero-extension.

Every change was tested in `tools/dynarmic-harness` with generators gen5-gen18: thousands of
programs, 0 failures.

### Measured on the console (04.30, against the last earlier log for each game)

| Game | Before | Now |
|---|---|---|
| Silksong | ~14 fps | 25-30 |
| Zelda: Echoes of Wisdom | 26-31 | 29-39 |
| Zelda: Link's Awakening | 19-22 | 24-28 |
| Mario Kart 8 (32-bit, translator unchanged) | 32-40 | 38-42 |

### Fixes
- **04.31**
  - Large vectors' (page table) pages come from direct memory when flexible memory is out. Fixed
    Bluey's boot crash.
  - The FPS counter turns red after a stall.
- **04.32:** 4 MiB default thread stacks (16 before), and a retry at 2 MiB on EAGAIN. Fixed Animal
  Crossing's "thread constructor failed" crash.
- **04.33:** the heap's overflow comes from direct memory first, so flexible memory stays free.
- **04.34:** no NPad::ControllerUpdate before the HID resource exists. Fixed the boot crash in
  Captain Toad and Little Kitty when a stick moves.
- **04.35 / 04.38:** CPU Unsafe off (Auto) turns off the A64 / A32 PS4 translator extras: a test
  switch.
- **04.36:** menu switch "Compiled GPU Macros" (off: interpreted).
- **04.37:** the boot trace is fsynced every second and the settings when saved, so logs survive
  a crash of the whole PS4.
- **04.39:** the GPU hang: the report now shows where the GPU stopped, and the app ends instead of
  the PS4 (below).
- **04.40:** a black box (`blackbox.txt`, synced 4 times a second) and a GPU stall note at 0.3 s
  (below).
- **04.41:** the boot trace synced within 50 ms; the GPU's first submissions after a loading screen,
  big GPU buffers and each RAM chunk in it (below).
- **04.42:** a test: geometry shader and tessellation draws (and their ring registers) skipped (below).
- **04.43:** the GS and tessellation rings set up as the PS4's system uses them: a 16 MiB off-chip
  tessellation buffer and the largest factor ring (`patches/mesa-ps4-rings.patch`), GS ring sizes
  through `sceGnmSetGsRingSizes(8 MiB, 8 MiB)`; those draws drawn again, still counted.
- **04.45:** multi indirect draws in the PS4's opcodes: Zelda's GPU hang fixed (below).
- **04.46:** the pre-flight record and the 2 ms GPU progress record (below).
- **04.47:** 04.46's slowdown and crash fixed (both the diagnostics'); "idle" is a 1 s gap (below).
- **04.48:** render targets checked; descriptors and the buffer list in the record (below).
- **04.49:** GPU work around a loading screen serialized (below).
- **04.50:** the mapping audit (below).
- **04.51:** the probe write and the skip test at Toad's address (below).
- **04.52:** the test skip of level 3's first 4-target scene draw (below).
- **04.53:** no HTILE on any depth buffer (`RADV_DEBUG=nohiz`): not the fix (below).
- **04.54:** image descriptor footprints checked; test arms (MRT0 only, no Z32F depth); full PS
  descriptor sets in the record; HTILE back on (below).
- **04.55:** the kitchen sink (`RADV_PS4_MITIGATE`, below).
- **04.56:** the kitchen sink plus trivial shaders for the scene draws after a loading gap (below).

## 04.56: trivial shaders for the scene draws

- **04.55's run:** every bit on (boot trace: `mitigations 0xff`, `RADV_DEBUG=nohiz,nofastclears`, new
  buffers zeroed; the scene draw's `SPI_SHADER_LATE_ALLOC_VS` 0, `RSRC3_VS` 0x3fffff, `IA_MULTI_VGT_PARAM`
  0x17007f). The CP stopped at offset 439, and this time the first 4-target scene draw itself never
  finished (before: it finished and the CP stopped 1-10 steps later). So none of those, and the cause is
  in that draw. Its vertex input: a prolog fetching a 32_32_32 position and a 2_10_10_10 SNORM attribute
  (the depth-only game draws that work use prologs too, without the 10-bit one).
- **04.56:** all of 04.55, and test arms 4 + 8 (`RADV_PS4_TEST_ARMS`, default 0xc) in the noted
  submissions: on every draw with 3 or more interpolants, the pixel shader is overwritten with one that
  exports zeros to each `SPI_SHADER_COL_FORMAT` target (no texture fetch, no interpolation), and the
  main vertex shader (VS user data 2-3, where the prolog jumps) with one exporting zero parameters and
  position (0, 0, 0, 1) (nothing rasterized). The prolog's vertex fetch still runs. Each replacement is
  in the boot trace; the note counts them. Host-tested (`desccheck.sh`).
- **Reading it:** level 3 through (empty scene): the shaders (then PS alone, then VS alone). Still
  stuck: the vertex fetch or fixed function.

## 04.55: the kitchen sink

- **04.54's run:** the footprint check misfired: the 1 KiB scanned from a set runs past it into the
  upload buffer's stale data (half T#s next to V#s: pitch 13857, slices 67-128), 952 "overruns", 376
  draws skipped. The scene draw at 455 was not among them and ran with the arms on: **MRT0 only, no
  depth** (33 target masks cut, 55 Z32F depths detached). The CP still stopped right after it (at
  457, the flush after a skipped copy). So not the 4 targets and not the Z32F depth.
- **What the scene draws do that the draws that worked don't** (the game's depth-only draws at
  25-277 included): export 5-7 parameters from the VS (`SPI_VS_OUT_CONFIG` 0x8/0xc; the others 0)
  read by 5-7 interpolants (`SPI_PS_IN_CONTROL` 5/7), with VS late alloc 28 (`SPI_SHADER_LATE_ALLOC_VS`
  0x1c, CU0 off for VS: RADV's (min_good_cu_per_sa - 2) x 4 with 9 CUs per SH) — late alloc with
  parameter exports is a known GCN deadlock, and the PS4's system may keep CUs for itself — and
  sample textures: a BC3 cube (128x128, 8 levels, 1D-tiled), a 1D R32F (64), a 2x2 BC1, and the
  256x256 BC1 the copy chain at 437-445 had just uploaded (0x43fdcf0d00).
- **04.55**, everything at once, each bit of `RADV_PS4_MITIGATE` (hex, default ff; 0 = none), in the
  boot trace at startup:
  - 1 (Mesa, `patches/mesa-ps4-mitigate.patch`): no CP DMA prefetch (shaders, VBO descriptors);
  - 2 (Mesa): no VS late alloc (LIMIT 0, VS on all CUs);
  - 4 (Mesa): conservative `IA_MULTI_VGT_PARAM` (SWITCH_ON_EOP, WD_SWITCH_ON_EOP, PARTIAL_VS/ES_WAVE_ON);
  - 0x10 (driver layer): new buffers zeroed at their first mapping;
  - 0x20 (frontend): `RADV_DEBUG=nohiz,nofastclears` unless settings.txt sets `RADV_DEBUG`;
  - still on: the serialized post-loading window, the record, the audit. The overrun findings are
    report-only now (the stale-data misfire), the 04.54 arms off.
- **Next:** one Toad level 3 run. Through: take the bits away one at a time (late alloc first).

## 04.54: not HTILE; three tests in one run

- **04.53's run:** HTILE was off (every depth draw's `DB_Z_INFO` without TILE_SURFACE_ENABLE:
  0x80000001 / 0x80000003), and level 3 stopped as before: the first 4-target scene draw (at 439 in
  this run) finished and the CP stopped 2 trace ids later, on the copy into 0x43fdcfc100 again. So not
  HTILE. Not occlusion counting or real depth tests either: the game's depth-only draws (Z16, offsets
  25-277, `DB_COUNT_CONTROL` 0x11000102, ZFUNC LEQUAL) all finished. The border color table
  (`TA_BC_BASE_ADDR`) is set and mapped. Left: the 4 color targets, the Z32F depth test, and the
  scene shaders' texture fetches (`IMAGE_SAMPLE`/`_L`, T#s loaded from 192-688 bytes into the
  descriptor set, past the record's 64 bytes). A wedged TA fits the stops best: the draw finishes and
  the next texture-unit work (a copy's buffer load, or 04.51 run 1's TC flush) never does.
- **04.54** (all in the noted submissions after a loading gap):
  - the descriptor check also checks each image descriptor's least footprint (level 0's pitch x
    height x bits x slices) against its buffer: far past it, the draw is skipped and logged;
  - test arms (`RADV_PS4_TEST_ARMS`, bits, default 3): 1 = `CB_TARGET_MASK` cut to MRT0, 2 = Z32F
    depth detached (`DB_Z_INFO` format to Z_INVALID); counted in each note;
  - the record keeps 1 KiB at every pixel shader user data pointer in the period's first record
    (64-byte chunks), so `gpu-preflight.py` decodes every T# (with its footprint) the scene reads;
  - HTILE back on (`RADV_DEBUG=nohiz` in settings.txt to turn it off).
- **Reading the next run:** a T# footprint line: likely the cause. Level 3 through without one: CB or
  DB (then one arm at a time). Still stuck: the texture side; the record has the descriptors.

## 04.53: HTILE off (the first real depth tests on a Z32F + HTILE buffer hang the GPU)

- **04.52's run:** the draw at 455 left out (logged), the stop moved from 456-466 to 499. Serialized,
  so exactly: the 4-target scene draw at 498 (the next of its kind, VS 0x44ffe1d600 / PS
  0x44ffde9800) finished, and the one right after it, at 499 (also 4 targets, VS 0x44ffe1d700 / PS
  0x44ffde9000), never did. The PS exports match the targets (MRT0/1/3 four FP16 channels, MRT2 two,
  `done` on the last); targets, depth, HTILE, index and vertex buffers all in live buffers.
- **What these draws have that every draw that worked on the same Z32F depth buffer (0x43ea508000,
  HTILE initialized with 0xfffc000f) lacks:** a real depth test (`DB_DEPTH_CONTROL` 0x700736:
  Z enable, Z write, ZFUNC LEQUAL), with an occlusion query counting (`DB_COUNT_CONTROL`
  0x11000102). The earlier ones (355-363) are RADV's clears and copies, ZFUNC ALWAYS, no query. A
  depth test makes the DB read HTILE (hierarchical Z); a write-only pass doesn't. So: a DB/HTILE
  hang, and the first draw of the kind can finish and leave the DB wedged for the next DB work (the
  meta flushes after a skipped copy in 04.51's run 1).
- **04.53:** the frontend sets `RADV_DEBUG=nohiz` (no HTILE on any depth buffer; `RADV_DEBUG=` in
  settings.txt brings it back). The 455-456 skip is off. If level 3 loads: HTILE on this depth buffer
  is it; then narrow it (Z32F only, or with queries only) to get HTILE back elsewhere.

## 04.52: not the memory; the stop follows level 3's first scene draw

- **04.51's two runs** (the first took the whole PS4 down, checkerboard and black in turns, a hard
  reset; the second's logs): the probe wrote 0x43fdcfc100 with a write confirm and the CP went on
  (so the GPU can write it). With the page's work skipped, run 1 stopped at the flush after the
  first skipped copy (no draw at all between the two trace ids: two `DMA_DATA` prefetches, the
  CB/DB meta flushes, `PS_PARTIAL_FLUSH`, `SURFACE_SYNC`), run 2 went on through the skipped chain
  and stopped at the next copy (0x43fdba3b00, bo 26 again). So not that memory.
- **The stop's place, all six recorded runs:** 456-466 trace ids into the batch, serialized or
  not (serialized runs are much slower, so it isn't time). In the batch, nothing writes into its
  IBs, outside live buffers or past a buffer's end; CP DMA all inside buffers, none into read-only
  ones; no scratch; occlusion queries (280 `ZPASS_DONE`, 8 RBs x 16 bytes a slot) normal; the
  Z32F depth's HTILE initialized (fill 0xfffc000f) and used fine at 355-363.
- **What's new at 455, the last thing before every stop:** the first draw of level 3's scene
  shaders in the batch, the first into **4 color targets** (RGBA8, RGB10A2, RG16, R11G11B10, 640x384,
  2D-tiled) at once, with Z32F + HTILE (compression off), 5 interpolants (`SPI_PS_IN_CONTROL` 5),
  an occlusion query around it. It finishes (its wait passes); the GPU stops within 1-10 steps.
  (04.51's skip also left out RADV copy-buffer dispatches whose raw pointers met data that looked
  like a descriptor in the page: a flaw of that test, off now.)
- **04.52:** that one draw left out (`RADV_PS4_SKIP_DRAWS=455-456`: draws and dispatches after
  those trace ids, in the first big submission after an idle GPU only; 0 = off). The page skip and
  the probe are off. If the stop moves on (the next such draw is at 575), that draw kind is it; if
  not, it's something before it.

## 04.51: a probe write and a skip test at 0x43fdcfc100

- **04.50's run:** the query reports GPU protection (self-test `prot 0x33`); the audit of 2789
  buffers found 0 wrong pages, five times over the load. bo 26 (phys 0x10820000): the stuck write's
  physical address 0x109dc100; no physical overlaps between buffers, nor with Eden's RAM chunks.
  The CP stopped at the same place (456 trace points into the batch). So the page is mapped as it
  should be, by its own memory, GPU-writable, shared with nothing we know of, and the first copy
  draw into it still never finishes, in all four recorded runs.
- **04.51, two tests in one run:**
  - **Probe:** before the first big submission of a period, an IB of our own: `WRITE_DATA` of 0 to
    `RADV_PS4_PROBE` (default 0x43fdcfc100) with write confirm, then trace id 0x7fff0001. The GPU
    stuck at the probe: it can't write that memory at all (a GPU-side mapping the kernel's query
    doesn't show). Past it: the CP can; the trouble is the draw's.
  - **Skip:** draws whose render target or descriptors touch `RADV_PS4_SKIP_TARGET` (default
    0x43fdcfc000:0x4000, the page) are skipped in the noted submissions (via the audit's holes).
    Level 3 loads: it's that spot. It goes down elsewhere: the record names the next stop.
  - Both `=0` in settings.txt to turn off.

## 04.50: the mapping audit

- **04.49's run (Toad level 3, serialized):** the batch (8040) recorded with a wait after every
  draw and dispatch; the CP passed trace id 201916 and never reached 201917: the one draw between
  them never finished. It is the first draw of the same chain: the 32x32 copy into **0x43fdcfc100**
  (bo 26, a 2 MiB garlic buffer, at +0x1bc100). Its source this time: bo 19 at +0xc2e700 (+0xece800
  in 04.48): the source moves from run to run, the destination is the same in all three. So: that
  memory, not the copy. By the driver layer's books (the live buffer list, no overlaps) it is fine.
- **04.50:** after a loading screen (the first 8 noted submissions of a period) every live
  buffer's pages are audited with `sceKernelVirtualQuery`: mapped, direct memory, the bo's own
  physical pages (`vq.offset`), GPU-writable (GPU-readable for a read-only bo) when the self-test
  shows the query reports GPU protection. Wrong pages become holes: a draw whose render target or a
  descriptor touches one is skipped and logged ("on memory the kernel doesn't map as the bo's"); the
  audit's summary goes in the boot trace. More than 8 bad buffers: not trusted, nothing skipped.
  Every unmap that runs over another live buffer is logged too (the likeliest way to punch such a
  hole). The record's buffer list has each bo's physical address.
- **Next:** one Toad level 3 run. A hole in bo 26: the cause (then the unmap lines say who made it);
  level 3 may load without that texture. No hole: the page is mapped as it should be, and the GPU
  still can't write it (next: its physical memory and GPU protection in detail).

## 04.49: GPU work around a loading screen serialized

- **04.48's runs:** Captain Toad at 48-55 fps again (04.47's fix) and Zelda 29 minutes at 25-40 fps
  (so 04.45's 10-12 fps was the diagnostics too), quit normally; four 2+ s GPU pauses in it, each
  went on (`gpu/gpu-hang.txt`, to look at).
- **Toad level 3 on 04.48:** the same six buffers, the same batch (8205: 900 draws, 193 dispatches),
  and the CP stopped at the same copy chain into 0x43fdcfc100: 461 trace points into the batch
  (04.46: 464), after 4 of its draws this time (7 then). The render target check found nothing; the
  record shows the destination in bo 26 (2 MiB garlic, at +0x1bc100), the source texel buffer in bo
  19 (Eden's 16 MiB staging buffer from boot, 1.25 MB to its end), no overlapping buffers, and
  register state identical to the 32x32 copy just before that finished (only addresses differ; CB1-3
  left set but disabled). Not a bad address, not bad state.
- **04.49:** after 0.25 s with nothing submitted, Mesa's `ac_ps4_flush_all_draws` (Crema's "Flush
  Caches" check: after each draw and dispatch the GPU waits for it and writes back and invalidates
  the caches) is on until the 60th submission after. Level 3's batch is recorded inside that window.
  The trace id in memory is then the last finished draw: the next one is the one that never
  finished. If level 3 loads instead, the crash needs overlapping work, and this is a fix.
  `RADV_PS4_SYNC_AFTER_IDLE=0` in settings.txt turns it off.

## 04.48: where Toad's level 3 stops the GPU (04.46's record)

- **The run:** Toad on 04.46 (still with its slowdown), straight to the level 3 load: the same six
  buffers (16, 16, 32, 16, 16 MiB garlic, 16 MiB onion) at 88.4-88.5 s, then submission 7167 (900
  draws, 193 dispatches, 17 register copies). The record took 510 ms; the progress record: the CP
  started 7167, passed trace id 178720 within 3 ms, and never moved again; the PS4 was gone ~0.1 s
  later (`tools/gpu-preflight.py` on the bundle).
- **The stop:** after a full wait (PS/CS partial flushes, `SURFACE_SYNC`), 7 draws of one 8-draw
  chain had gone out and the CP stood at the 8th: the 7 contexts of the CP all held by draws that
  hadn't finished, so the first of them never did. The chain: RADV's buffer-to-image copy (blit2d:
  the PS fetches `buffer_load_format_xyzw` at x + y * pitch from a texel buffer, one draw per mip
  level) into a 32x32 RGBA16F texture at 0x43fdcfc100 with eight levels (32 ... 1, then two more
  1x1: the guest's level count), 1D-tiled (tile mode 13). An identical chain just before (64x64,
  nine levels, at 0x43fdcf0d00, right below it in memory) finished.
- **Not it:** the shaders (the same ones ran 450 times in 7167), the descriptors' bases and ranges
  (the 04.44 check: 0 wild, 0 overruns). **Never checked:** where the draws write.
- **04.48:** the descriptor check also checks every draw's render targets: color targets (base, and
  the slices' size from `CB_COLOR*_SLICE`/`VIEW`), CMASK/FMASK when in use, depth/stencil and HTILE;
  one in no live buffer, or far past its buffer's end, is logged and its draw skipped. The record
  keeps 64 bytes at every user data pointer (the descriptor sets) and the live buffer list, and the
  decoder checks the stuck work's targets and descriptors against it.
- **Next:** one Toad level 3 run on 04.48. A logged render target: the copy writes outside its
  memory (an Eden or driver-layer bug about that texture), and level 3 may load with the copy left
  out. None: the record names the buffers and descriptors of the stuck copy.

## 04.47: the diagnostics' slowdown and crash

- **04.46's run (Captain Toad level 1):** 11-38 fps (04.45: ~52), then Eden's crash page: SIGSEGV
  at elf 0x3bf477 = `ps4_check_set` (the 04.44 descriptor check) reading 0x44fdfac00c.
- **The slowdown:** the "GPU idle 0.5 s" that starts 40 noted submissions was the stall thread
  seeing the GPU done at each of its 50 ms looks for 0.5 s. A game whose GPU idles between frames
  met that every second or so: Zelda's 04.45 run had 220 such periods (8762 noted submissions, each
  walked and checked; most likely its 10-12 fps too), Toad's 04.45 run 32 in 60 s. 04.46 added the
  pre-flight record (MiBs written and synced per period), which slowed Toad further and so met it
  more often. Now: a 1 s gap with nothing submitted and the GPU caught up (`RADV_PS4_NOTE_GAP_MS`),
  the note's own time not counted; the record takes the first 16 big ones of a period.
- **The crash:** `ps4_check_set` read up to 1 KiB from a descriptor set pointer without checking it
  was mapped, and bounded it by `b->size` (a pointer into a mapped tail wrapped that). Now bounded by
  the live range's end and checked with `ps4_mapped` first.
- **Next:** the same Toad run (levels 1, 2, then 3) at 04.45's speed; level 3's record then names
  the command the GPU was on.

## 04.46: the pre-flight record (Captain Toad level 3)

### 04.45's runs
- **Zelda: Echoes of Wisdom made it past the cliff scene:** the hang is fixed. It then ran at 10-12
  fps (boot trace at 390 s: GPU busy 7%, but the CPU waited for it 29 times a second, 20.6 ms each,
  59% of the time). Next after Toad: why the waits are that long with the GPU that idle.
- **Captain Toad level 3 still takes the PS4 down,** the same way as 04.41's: the GPU idle 1.9 s
  (loading), six big buffers (16, 16, 32, 16, 16 MiB garlic, likely Eden's upload staging buffers,
  and a 16 MiB onion one, a download buffer), then submission 7941: 458 draws, 138 dispatches, 4
  `WAIT_REG_MEM`, 17 register copies (`COPY_DATA` memory to register: RADV's fast clear value loads;
  no other of 1288 noted submissions had more than 3), descriptors all in live buffers. 7942-7949
  (level 3's first frame among them) were queued by 61.517 s; the black box's next line (61.66 s)
  never came. So the PS4 went down 0.1-0.25 s after 7941 reached the GPU.
- The black box's "NOTHING FINISHED FOR 82 ms" at 61.408 s was the note's own time (walking 7941
  before it went to the GPU), not the GPU's: nothing in the log says how far the GPU got into 7941.
- The note's walk was cut short at 4096 packets (16384 now) and its uconfig list repeated
  `VGT_PRIMITIVE_TYPE` (the register index bits): fixed.

### 04.46
- **`gpu/preflight.txt`:** each submission noted after an idle GPU that has 8 or more draws and
  dispatches: every IB's dwords (top-level, nested, chained, in the CP's order) and the code of each
  shader it sets (once a period), written and **synced before the submission goes to the GPU**. At
  most 6 MiB a period; the previous period's in `preflight-previous.txt`. The boot trace says how
  long each took. `RADV_PS4_PREFLIGHT=0` in settings.txt turns it off.
- **`gpu/progress.txt`:** every 2 ms while noted submissions run (and 3 s after the last), synced
  every 10 ms: completed, handed to the system, the last the CP started (its start timestamp), and
  the trace id the CP passed last, with the longest sync so far.
- **`tools/gpu-preflight.py <eden-ps4-logs.txt>`:** walks the record as the CP does to the last trace
  id in the progress record and prints the packets from there, decoded (register names from
  `tools/gfx7-registers.txt`, Mesa's), with the shaders in effect. `tools/gpu-hang-shaders.py` takes
  the record's shaders too. Host test: `tools/gpu-tests/preflight.sh`.
- **Next:** one Toad level 3 run with 04.46: the progress record's last line and the record name the
  command the GPU was on.

## 04.45: the GPU hang found (confirmed: Zelda runs past it)

- **Cause:** RADV writes multi indirect draws as opcode 0x38 (indexed) / 0x2C in the desktop layout
  (with draw index register and count address). On the PS4's CP firmware 0x38/0x2C are a *short* form
  (Sony's `sceGnmDrawIndexIndirectMulti`: offset, base vertex reg, start instance reg, count, stride,
  initiator); the desktop layout is 0x9D/0x9C there (`sceGnmDraw(Index)IndirectCountMulti`, base mode
  too; from shadPS4). The CP read `0x80000052` draws with a 1-byte stride: ~2 billion draws through
  whatever memory follows the arguments.
- **Evidence:** Zelda on 04.43 hung; the trace id in memory (2627226) and the next (2627227, not
  reached) have only prefetches, `SET_BASE` and one `DRAW_INDEX_INDIRECT_MULTI` between them; 04.38's
  Zelda hang had one too. The PS4 survived that run (04.39's exit on a hang works).
- **Fix:** `patches/mesa-ps4-indirect-draws.patch` (0x9D/0x9C on the PS4). Also: the driver layer's
  walkers stopped at `PKT3_NOP_PAD` (0xffff1000), so 04.42-04.44's counts and checks saw only parts of
  submissions; fixed.
- **Next:** Zelda past the point where it hung, and Captain Toad's level 3.

## Where we are: the open problem (before 04.45)

**The GPU hangs at heavy loading points and takes the whole PS4 down** (CE-36329-3, black or
checkerboard screen, sometimes a hard reset).
- Seen in Captain Toad level 3 and in Echoes of Wisdom just after the cliff scene in the intro.
  Not seen in Mario Kart.
- **Ruled out:**
  - the CPU translator extras (CPU Auto, both A32 and A64)
  - the GPU macro JIT
  - Draw Shortcuts
  - asynchronous shaders, resolution, GPU accuracy
  - memory (GBs of direct memory free)
- **The Zelda log has the evidence:** Mesa's `ac_ps4_drm.c` reported "GPU hang? waiting for
  30753, completed 30752". `gpu/gpu-hang.txt` holds that submission: 135 draws, 199 dispatches.
  - Its only progress markers were Eden's `WaitForIdle` (`vkCmdSetEvent` + `vkCmdWaitEvents` on one
    event at 0x43fdb4c000, never reset): every one writes 1, so the report couldn't show which draw
    or dispatch never finished.
  - The system crash came about 30-40 s later, while Eden was still waiting.

### 04.39: both next steps from 04.38, in one build (`port/mesa/ac_ps4_drm.c`, `patches/mesa-ps4-trace-ids.patch`)
1. **Find the cause:** RADV's trace ids are on (`RADV_PS4_TRACE=ids`): a number, unique per write,
   as the command processor reaches each draw, dispatch and cache flush. The hang report's new first
   section, "where the GPU stopped", names the last trace id passed, the wait the CP is stuck at, the
   suspect draws and dispatches with their shaders, and writes those shaders' code to
   `gpu/hang-shaders.txt` (`tools/gpu-hang-shaders.py`).
2. **Stop the system crashes:** `RADV_PS4_HANG_EXIT=1`: if the GPU hasn't finished anything 2 s
   after the report started (about 4 s into the hang), `abort()`. The PS4 may then close just the app.

### 04.39's run (Captain Toad level 3): no hang report
- The PS4 went down as before, but 04.39's hang path left nothing: no "GPU hang?" boot trace line
  (synced, 2 s into a GPU wait), `gpu-hang.txt` still 04.38's Zelda one.
- The boot trace ends at 64.8 s, ~4 s after overflow heap 15 (the 32-bit translator's 23 MiB
  allocation; Mario Kart makes it too and lives). All four Toad runs end within 4-10 s of it: it marks
  level 3's loading, not the cause.
- So in Toad nothing waited on the GPU for 2 s before the PS4 died, or it died for another reason.
  Zelda's (the GPU stopped, the PS4 lived ~40 s more) may be a different fault.
- Never tried for Toad: CPU Cores 6 (with 7, Eden's shader compilers and services also run on the core
  the system shares), Fastmem off, Thread Placement Free, Switch Memory all up front.

### 04.40: what the next crash leaves
- `blackbox.txt`: 4 lines a second (GPU submitted/finished, how long it has finished nothing, trace
  id, busy share, frames, the GPU thread's stage, memory), fsynced each time, a 2-minute ring.
- A stall thread in the driver: after 0.3 s with nothing finished, `gpu/gpu-stall.txt` ("where the
  GPU stopped") and a synced boot trace line, without anything having to wait.

### 04.40's runs (Captain Toad level 3, three of them): not the GPU
- Settings: 7 cores, then 6, then 7; Fastmem off; compiled macros; resolution 50%. All three went down.
- The black box: for the last 1.8 s the GPU was idle and caught up (0% busy, every submission done),
  no new frames (level 3 loading), the RAM growing. No stall note (nothing was waiting on the GPU).
- The last line of both the black box and the boot trace (80.288 s): the macro JIT's 10th 64 KiB
  block at `0x100c098000`, the same in all three runs; the PS4 went down within 0.25 s.
- 04.37-04.39's runs with interpreted macros (no such blocks) went down at level 3 too: the block is
  most likely just the last thing logged before level 3's first frames.
- So CPU Cores and Fastmem are ruled out too, and the swapchain (never recreated: fixed 1280x720).

### 04.41's run: level 3's first frame
- The usual settings. The screen flickered between garbage and black; a hard reset.
- Level 3 loaded (six 16-32 MiB GPU buffers: two of them the size of RADV's GS rings on the PS4),
  its first frame reached the GPU at 69.520 s (183 draws, 2 dispatches), the GPU finished nothing
  after 69.491 s, and the PS4 was gone within ~0.1 s: the GPU, and fast.
- "Switch Memory: All 4 GB up front": `std::bad_alloc` at 30 s (no room for the C heap's overflow).
  Not usable for Toad.

### 04.42: the test
- Draws with a geometry shader or tessellation, and RADV's writes of those rings' registers (the
  system's on the PS4), become NOPs (`RADV_PS4_SKIP_GS_TESS=1`, the default in this build), and are
  counted in the boot trace.
- Toad level 3 runs (with something missing): they're the cause; then the fix is setting the rings
  up as the PS4 wants. It still goes down with none counted: not them. Down with some counted and
  skipped: not them either.

### 04.43's run: not GS or tessellation
- `sceGnmSetGsRingSizes(8 MiB, 8 MiB)` returned 0; 2288 noted submissions had no GS or tessellation
  draw and no ring register write. Level 3 went down as in 04.41.

### 04.44: bad descriptors
- After a loading screen, each draw's and dispatch's descriptors are checked against the live GPU
  buffers; a wild one (or a range far past its buffer) is logged and its draw skipped. The notes carry
  a census of global-state packets (config and register writes, GDS, streamout, unusual packets).
- Next: one Toad level 3 run. Wild descriptors logged and level 3 runs: that's it, then find which
  shader and why (Eden's storage buffer tracking, or the descriptor buffer path). None logged and it
  goes down: the census lines of level 3's first frame against the frames before it.

### What other emulators and ports found (research, after 04.42)
- **Mesa issue 3168 (2020): yuzu, Captain Toad, AMD GCN (R9 Fury, Linux), "the GPU hangs and I need
  to reboot" on entering stage 2; fine on Windows.** Closed as fixed by Mesa !5632 ("radeonsi: bump
  SI_NUM_SHADER_BUFFERS to 32": yuzu's shaders use more than 8 storage buffers) and !5167
  (gl_Layer/gl_ViewportIndex in the VS). On the related Super Mario Odyssey hang (issue 3057) the
  Mesa developer found "an infinite loop in a compute shader" because "this shader uses more than 8
  storage buffers"; it hung on gfx9, not Navi. So: Captain Toad's shaders bind many storage buffers
  (the Switch's global memory), and a bad buffer descriptor took a GCN machine down. RADV has no 8-slot
  limit, but Eden on the PS4 uses VK_EXT_descriptor_buffer (raw GPU addresses in descriptors): the
  next suspect if 04.42 doesn't settle it (a run with descriptor buffers off).
- **yuzu progress reports:** the Switch's geometry shader passthrough (an NVIDIA feature, for viewport
  and layer selection) is emulated with real geometry shaders on AMD, so a Switch game can run GS on
  the PS4 without having one (our logs: "Device doesn't support extension
  VK_NV_geometry_shader_passthrough"). Tessellation crashed AMD's Windows driver until yuzu forced the
  PATCH_LIST topology.
- **shadPS4 (Sony's libSceGnmDriver as reverse-engineered):** `sceGnmSetGsRingSizes` takes 4-8 MB,
  1 MB aligned, and sets VGT_ESGS/GSVS_RING_SIZE itself (Crema's 16 MiB GS rings cover 8 MB). The
  tessellation factor ring is one system buffer at a fixed system address
  (`sceGnmGetTheTessellationFactorRingBufferBaseAddress`), and the off-chip buffer is 8 MiB
  (`sceGnmGetOffChipTessellationBufferSize`). RADV's tessellation buffer on Liverpool is ~2.3 MiB (2
  SEs x 36 workgroups x 32 KiB + 54 KiB of factors), with no PS4 margin: if the system's
  VGT_HS_OFFCHIP_PARAM / VGT_TF_MEMORY_BASE stay (as the GS ring sizes do), the HS writes up to
  ~5.7 MiB past it and the VGT reads factors from the system's ring. A fix to make either way: an
  8 MiB-or-more off-chip ring, and the system's factor ring.
- **Eden4 (github.com/xatyy/eden4):** no GS or tessellation handling; two games tried. PS4 Pro mode
  "hung the GPU and crashed the whole console" (RADV describes the base GPU): a GPU configuration
  that doesn't match the system's is a known way to take the console down.

### Also open
- **Little Kitty, Big City:** a black screen. The game's allocator traps
  (`nnmem_nlib_debug_break`) after reads at 0 and at unmapped addresses, 16 times. Not the
  translator extras. Probably an emulation gap.
- **Bluey:** runs, but at 12-16 fps.
- **Shelved:** `exit_pc_in_register` (04.31 WIP). It's in `git stash` in `~/eden-pptc`.
  - Indirect jumps alone: unity -0.35%, not worth it.
  - Returns need the return stack's placeholder entries to store the PC first.

## Frame rates of every game tested (from all console logs, 04.01-04.38)

Each game has two rows: "best" is the run with the highest average, "latest" is the most recent run.

- **How the numbers were taken:** from the boot trace's `speed:` lines, each a 10-second average. The first three samples (boot) and samples under 1 fps (loading screens, hangs) are left out.
- **Version:** the build that exported the logs. A run in an upload can be from that build or the one just before.
- **Caveats:** menus count too (they often run at 55-60 fps), and scenes differ between runs.
- **Captain Toad:** its runs end at level 3's GPU hang.

| Game | Run | Version (logs exported by) | High | Low | Average | 10-s samples |
|---|---|---|---|---|---|---|
| Animal Crossing New Horizons | best | 04.31 | 56.2 | 23.9 | 33.8 | 26 |
| Animal Crossing New Horizons | latest | 04.34 | 58.8 | 13.8 | 31.0 | 17 |
| Bluey's Quest for the Gold Pen | best | 04.31 | 50.6 | 3.2 | 18.9 | 17 |
| Bluey's Quest for the Gold Pen | latest | 04.33 | 47.6 | 3.7 | 16.8 | 16 |
| Captain Toad Treasure Tracker | best | 04.38 | 59.9 | 55.4 | 58.8 | 4 |
| Captain Toad Treasure Tracker | latest | 04.38 | 59.9 | 55.4 | 58.8 | 4 |
| Hollow Knight Silksong | best | 04.30 | 58.6 | 11.7 | 30.7 | 22 |
| Hollow Knight Silksong | latest | 04.30 | 58.6 | 11.7 | 30.7 | 22 |
| Mario Kart 8 Deluxe | best | 04.10 | 58.4 | 34.0 | 46.2 | 8 |
| Mario Kart 8 Deluxe | latest | 04.38 | 57.6 | 26.9 | 41.7 | 15 |
| The Legend of Zelda Echoes of Wisdom | best | 04.30 | 51.2 | 22.1 | 33.7 | 18 |
| The Legend of Zelda Echoes of Wisdom | latest | 04.38 | 58.5 | 7.3 | 31.4 | 24 |
| The Legend of Zelda Links Awakening | best | 04.30 | 33.8 | 17.6 | 26.3 | 9 |
| The Legend of Zelda Links Awakening | latest | 04.30 | 33.8 | 17.6 | 26.3 | 9 |
