# Eden for PS4: status (after 04.38)

Install **04.38** (newest). Packages are built to `out/EdenPS4-<version>.pkg`. They are not in git.
The patches are in `eden-ps4/patches` (up to 0143). The Eden tree is `~/eden`, branch `ps4`. The
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

## Where we are: the open problem

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
  30753, completed 30752". `gpu/gpu-hang.txt` holds that submission.
  - Its IB 1 ends with RADV's wait-for-idle pairs (EVENT_WRITE_EOP 0 then 1, then WAIT_REG_MEM
    == 1, all at 0x43fdb4c000) and meta compute dispatches.
  - Every pair writes the same address, so the report can't show which draw or dispatch never
    finished.
  - The system crash came about 30 s later, while Eden was still waiting.

### Next steps (Mesa: `/root/cemups4-build/src/mesa/src/amd/common/ac_ps4_drm.c`)
1. **Stop the system crashes:** after `ps4_report_hang()`, end the app (abort, so the crash
   screen shows) instead of waiting. The PS4 may then close the app cleanly instead of crashing.
2. **Find the cause:** make each EOP marker in a submission write a unique value, so the hang
   report names the draw or dispatch that hung. Then fix it in Eden's GPU emulation or in RADV.

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
