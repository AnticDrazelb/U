# Taking xemu's hardware models into xbox-core

The owner asked (2026-11-10) for xemu's device models wholesale instead of piece by piece, with no
round trips to the console between pieces: port, verify here in depth, ship once. xbox-core becomes
GPL-2.0-or-later (agreed). xbox-core's base stays: the Xbox's code runs natively (32-bit mode on the
PS4's CPU) on xbox-core's own kernel (HLE); QEMU's CPU translator and the real BIOS/MCPX ROM are
neither fast enough on the PS4 nor available.

## Done
- APU, whole (registers, frame thread, VP, GP/EP DSPs on the DSP56300 interpreter): a8cf63e.
- nvnet (no cable: link down): 49a08e4.
- Shared: `src/third_party/xemu/shim` (QEMU/glib/SDL stand-ins), `src/xemu_glue.cpp` (guest physical
  memory, block DMA, IRQ lines, settings), `shim/xb_qemu_timer.c` (QEMU's ms/ns timers on a thread).

## Kept, and why
- AC97: xemu's (QEMU's ac97.c) never transfers the S/PDIF box (index 7) and has no nForce bit 4
  (NVSPINT, 0x10) in GLOB_STA: exactly what got Halo past DirectSound's 11.8M-read wait (a51ebcb).
  Ours stays; audit it against ac97.c register by register (task).

## NV2A (in progress)
xemu: hw/xbox/nv2a (nv2a.c, pfifo.c, pgraph/pgraph.c, pgraph/vk/*, pgraph/glsl/*, ptimer/pcrtc/...),
~20k lines for the Vulkan path. Dependencies, pinned to xemu's revisions (~/emu/deps):
glslang e1b562a, volk 3ca312a, VulkanMemoryAllocator 0f1a757, SPIRV-Reflect f3e261f,
nv2a_vsh_cpu 89315e8, xxHash v0.8.3.

Plan:
1. Build the dependencies (desktop and PS4): glslang (glslang + SPIRV, no HLSL/optimizer), volk,
   VMA, SPIRV-Reflect, xxhash, nv2a_vsh_cpu.
2. Vendor hw/xbox/nv2a minus gl/ (and the GL display interop), plus QEMU's util headers it needs
   (queue.h, lru.h, fast-hash, mstring).
3. Shims: glib (GArray, GByteArray, g_autofree, g_strdup_printf), qemu_event, VGA registers
   (PRMCIO/PRMVIO's vga_ioport_*), memory regions (vram = guest RAM, ramin), PCI/IRQ 3, timers.
4. Dirty memory (QEMU's per-page bitmaps for DIRTY_MEMORY_NV2A and _NV2A_TEX, maintained by its CPU
   translator; nothing tracks native writes here): per-page content hashes per client plus
   set_client_dirty marks; test_and_clear compares and refreshes. (Write protection is the faster
   alternative if hashing costs too much on the PS4; it must then handle host-side writes too.)
5. Display: at vblank the renderer downloads the displayed surface into guest RAM
   (download_surfaces_in_range_if_dirty); xbox-core's presenter shows guest RAM as now.
6. Adapter in place of src/nv2a.cpp for GPU builds (the no-GPU packages keep xbox-core's renderer:
   xemu's null renderer draws nothing).
7. Verify: nxdk_pgraph_tests against the console's captures (tests/pgraph/run.py), side by side with
   xbox-core's renderer (2612/3110 in software); the regression set; the FreeBSD VM tests; then
   build for the PS4 (RADV).

## USB (after the NV2A)
QEMU's hcd-ohci.c + USB core/bus/desc + hub + xemu's xid/xid-gamepad (~6.3k lines, QOM-heavy).
