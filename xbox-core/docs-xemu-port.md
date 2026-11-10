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
- Verification: nxdk_pgraph_tests against the console's captures (tests/pgraph/run.py), compared
  per suite with xbox-core's own renderer (2612/3110 in software, run-sw5).

