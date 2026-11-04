# Eden for PS4: status (after 04.46)

Install **04.46** (newest). Packages are built to `out/EdenPS4-<version>.pkg`. They are not in git.
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
