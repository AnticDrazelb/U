# xemu's devices in xbox-core

xbox-core's APU is xemu's (https://github.com/xemu-project/xemu, `hw/xbox/mcpx/apu/`), whole: `apu.c`
(registers, frame thread, pacing), `vp/` (the voice processor: voices, ADPCM, filters, HRTF, the
resampler), `dsp/` (the GP and EP DSPs: `gp_ep.c`, and the DSP56300 C interpreter, adapted from
Hatari's DSP 56001 emulation), `monitor.c`, `debug.c`. The files keep their own headers: GPL-2.0-or-
later or LGPL-2.1-or-later. xbox-core as a whole is under the GPL, version 2 or later (`COPYING`).
The voice processor's resampling is libsamplerate (`src/third_party/libsamplerate`, BSD-2-Clause).

Changes for xbox-core:
- `apu.c`: QEMU's PCI device, reset and snapshot plumbing replaced by `xb_mcpx_apu_init`,
  `xb_mcpx_apu_read` and `xb_mcpx_apu_write` (the window dispatched by offset as QEMU's memory
  regions did); the frame thread is let run at start.
- `monitor.c`: the frames go to xbox-core's mixer (`xb_apu_sound`), not an SDL stream; pacing by the
  clock.
- `dsp/dsp.c`: the C interpreter only (xemu's JIT needs its dsp56300 library).
- `dsp/interp/dsp_cpu.c`, `dsp/gp_ep.c`: memory accesses bounds-checked where xemu asserts (xbox-core
  builds with asserts off).
- `shim/`: what the code uses of QEMU, glib and SDL: threads on pthreads, atomics, the clock, guest
  physical memory and the interrupt line through xbox-core (`src/apu.cpp`), trace points compiled out.
- Settings: xemu's defaults (its DSP option off, HRTF on), two voice workers.
- The front end's trap (`apu.c`, `vp/vp.c`; 2026-11-11): FETINTSTS latched once where the idle-voice
  trap is taken (xemu re-derived it from FECTL, so DirectSound's acknowledgement before resuming the
  front end brought it back and the next delivery replayed a released voice); one idle voice trapped
  on at a time, after the frame, under the big lock, a declined one giving way to the next; methods
  written while the front end is trapped or halted queued until it runs (PIO_FREE reports the room);
  an idle voice with the trap not forced skipped (xemu asserted).
- `monitor.c`: the frame thread paced by what xbox-core's mixer holds of its sound (xemu's throttle()
  with its SDL stream), 4 to 6 EP frames ahead.
- `nv2a/pgraph/vk/buffer.c`: buffers for the largest single draw (inline vertices 128 MiB, indices
  16 MiB), not xemu's ten and hundred of them (the PS4's GPU memory).

## Also taken whole since (see docs-xemu-port.md)
- `hw/xbox/mcpx/nvnet`: the network adapter (link down: no network behind it).
- `hw/xbox/nv2a`: the GPU, with its Vulkan renderer (not its OpenGL one, nor the GL interop with
  xemu's UI), linked with glslang, volk, VMA, SPIRV-Reflect, xxHash, nv2a_vsh_cpu
  (`src/third_party/*`, each with its licence). xbox-core additions are marked `xbox-core:` in the
  sources (device glue in nv2a.c, the idle sync, the renderer's start-up result, unimplemented texture
  formats, volk from a linked driver).
- `hw/usb` (QEMU's OHCI, core, descriptors, hub; bus.c's parts in xb_bus.c) and `hw/xbox/xid*`
  (xemu's Xbox controller); xb_usb.c builds the controllers and player 1's hub and pad.
