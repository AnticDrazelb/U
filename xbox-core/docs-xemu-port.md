# Taking xemu's hardware models into xbox-core

The owner asked (2026-11-10) for xemu's device models wholesale instead of piece by piece, with no
round trips to the console between pieces: port, verify here in depth, ship once. xbox-core becomes
GPL-2.0-or-later (agreed). xbox-core's base stays: the Xbox's code runs natively (32-bit mode on the
PS4's CPU) on xbox-core's own kernel (HLE); QEMU's CPU translator and the real BIOS/MCPX ROM are
neither fast enough on the PS4 nor available.

## Done
- APU, whole (registers, frame thread, VP, GP/EP DSPs on the DSP56300 interpreter): a8cf63e.
- nvnet (no cable: link down): 49a08e4.
- NV2A, whole (PMC, PBUS, PFIFO, PGRAPH + Vulkan renderer and shader generators, PTIMER, PCRTC,
  PRAMDAC, PVIDEO, PFB...): 80ad128 on; the default where Vulkan is built, XB_NV2A=own for
  xbox-core's; a renderer that can't start falls back to xbox-core's (e05c88e); the GPU reads idle
  only once its work is done (a5787c7); unimplemented texture formats non-fatal (eeced79).
- USB: QEMU's OHCI, core, hub + xemu's XID gamepad, player 1 as xemu plugs one in (hub in port 3,
  pad in its port 1): f538340; XB_USB=own for xbox-core's.
- AC'97: kept and audited against QEMU's ac97.c (c5983ec).
- Shared: `src/third_party/xemu/shim` (QEMU/glib/SDL stand-ins: object model, qdev, timers on a
  thread, the big lock, memory regions with page-hash dirty tracking, VGA registers, DMA, iovecs),
  `src/xemu_glue.cpp` (guest physical memory, block DMA, IRQ lines, settings, the log).

## Changed in xemu's APU for the console's behaviour (2026-11-11)
Halo died in DirectSound's interrupt routine (ISTS 0x11, a null voice object at 0x1958d2): xemu ORs
FETINTSTS back into ISTS while FECTL reads trapped, and DirectSound acknowledges ISTS before it lets
the front end run, so the next delivery replayed a trap whose voice was gone. Now (marked xbox-core:
in apu.c, vp/vp.c, monitor.c; the same findings as other projects running the XDK's DirectSound):
- FETINTSTS latched once where the idle-voice trap is taken; one idle voice trapped on at a time,
  after the frame, under the big lock; a voice the routine declined gives way to the next.
- Front-end methods written while it is trapped or halted wait in a FIFO and run when it runs again;
  PIO_FREE reports the room left. An idle voice with the trap not forced is skipped (xemu asserted).
- The frame thread paced by what the mixer has queued (xemu's throttle() with its SDL stream).
Tests: tests/apu-trap-test (16 replays before, none now), the APU test's sound through a simulated
console device (XB_WAV_DEVICE=1), both in the regression set.

## The APU's speed (2026-11-11)
48 voices took 67-95% of a fast desktop core (sinc resampling half of it, worker handoffs 1500 times
a second, the HRTF's modulo indexing): a busy scene on the PS4's slower cores couldn't keep up with
real time. Lagrange resampling, voices mixed in place and the mirrored HRTF line make it 16%. The
stats line shows the frame thread's busy share and its voices (tests/apu-load-test to measure).

## The APU's DSPs (2026-11-11)
xemu's default leaves the GP and EP DSPs out of the sound (its DSP option off: they get 1000 cycles a
frame and the voices are mixed directly; no reverb or other effects of DirectSound's DSP image).
Settings > Sound effects (XB_APU_DSP=1) turns them on: each runs until its frame halts, at most
twice a frame's cycles (a program that never halts can't hold the frame thread), and if the frames
take more than 95% of real time for 3 s they go off again (logged). The interpreter does about 580
million DSP cycles a second on a desktop core; untested with DirectSound's own image (only a game
has it).

## The Xbox's single processor (2026-11-11)
Interrupt routines and DPCs run with every other guest thread held (StopGuestThreads: the sample
signal holds a thread in guest code where it is; others are held before they go back into guest
code). IRQL holds DPCs off at dispatch level and routines above it; KeSynchronizeExecution runs its
routine at the interrupt's level. DirectSound's voice lists depend on both.

## Kept, and why
- AC97: xemu's (QEMU's ac97.c) never transfers the S/PDIF box (index 7) and has no nForce bit 4
  (NVSPINT, 0x10) in GLOB_STA: exactly what got Halo past DirectSound's 11.8M-read wait (a51ebcb).
  Ours stays; audit it against ac97.c register by register (task).

## NV2A notes
- Dirty memory: QEMU's per-page bitmaps came from its CPU translator; here a page is dirty for a
  client when its contents' XXH3 hash changed since that client asked, or it was marked
  (shim/xb_memory.c). xemu's non-TCG path (tcg_enabled() false, as under KVM/HVF) downloads surfaces
  to RAM as drawing to them ends.
- Display: at vblank the renderer downloads the surfaces drawn (its pre-snapshot path); xbox-core's
  presenter shows guest RAM as before.
- Idle: reading PGRAPH's status with the push buffer drained finishes the renderer's work first
  (reports written, surfaces downloaded), as the console's GPU would have.
- BIOS state xbox-core supplies (xemu runs the BIOS): PFB_CFG0/1.
- Verification: nxdk_pgraph_tests against the console's captures (tests/pgraph/run.py), per suite
  against xbox-core's own renderer. xemu's (Vulkan on lavapipe): about 2547/3110 (81.9%); xbox-core's
  (software, run-sw5): 2612/3110 (84.0%). xemu's better on 17 suites (shadow comparator 288 vs 204,
  W_param 83 vs 42, Depth_Clamp 40 vs 26, W_buffering 239 vs 233, cube-from-2D, combiners...), xbox-
  core's on 31 (Blend_surface 32 vs 6, cubemaps 66 vs 37, signed textures 17 vs 1, stencil 8 vs 0,
  fog, line width, front face, border texels...). xemu's renders Halo on PC; it is the default, and
  Settings > Graphics (settings.txt: nv2a 1) or XB_NV2A=own picks xbox-core's.
- One job of the comparison ended silently once (in Vertex_shader_swizzle_tests, which passes 29/29
  alone): not reproduced; watch for it.


## xemu's renderer on the PS4 (2026-11-11)
Its buffers were sized for a PC (about 4.6 GiB: ten largest inline batches staged twice, 100 index
batches): now the largest single draw (128 MiB inline, 16 MiB of indices, 128 MiB compute), about
0.75 GiB. The step file covers its start-up and its first half minute of drawing.

## The device's memory as a bus (2026-11-11)
xbox-core's guest RAM is two blocks: the contiguous memory (physical 0-64 MiB, at 0x80000000) and the
RAM a program's image and heap are in (its own physical addresses, above 64 MiB). xemu's devices
reach both through xb_phys_read/write and the ld*/st*_phys shims; two of xemu's APU paths indexed
its RAM pointer directly, which covers the contiguous memory only: an ADPCM stream's packets (a
DirectSound client's packet from its heap crashed the emulator) and the GP's and EP's scratch and
FIFO memory. Both go through the bus now (the APU test's voice 105 covers the first).

## xemu's renderer: its start-up steps and a hang (2026-11-11)
The step file (xemu-nv2a-step.txt) names each step from the device's state through PGRAPH's, the
PFIFO thread and the renderer's state to the Vulkan instance, buffers, shaders, pipelines and test
draws. A renderer that hasn't started in a minute (a driver call that never returns) is left where
it is and xbox-core's own NV2A draws (XB_VK_HANG=1 and XB_NV2A_START_MS make one for a test).
For looking into its drawing: XB_VK_SHADERS=folder writes each shader's GLSL there, XB_VK_DRAWS=1
logs each inline-buffer draw (attributes, first vertex), XB_VK_FAIL_BUFFER=n refuses buffer n.

## Verified before shipping (2026-11-10, commit 6ab6dee)
- Regression 13/13 normally, with XB_TEST_FS_AS_PS4=1, and with XB_USB=own / XB_NV2A=own.
- FreeBSD VM (the PS4's OS family): kernel, SEH and disc tests 0 failures, triangle runs; built
  with xemu's APU, NIC and USB (its build has no Vulkan: xbox-core's NV2A there).
- nxdk_pgraph_tests on xemu's NV2A: about 81.9% of the console's captures (xbox-core's 84.0%).
- All three PS4 packages build; the 1 GiB GPU one has xemu's NV2A (default, Settings > Graphics to
  switch), the others xbox-core's NV2A with xemu's APU, NIC and USB.
- Not testable here: xemu's renderer on the PS4's RADV and its memory there (on lavapipe it uses about
  240 MiB more than xbox-core's GPU path); a start that dies inside its start-up is followed by one
  with xbox-core's NV2A.
