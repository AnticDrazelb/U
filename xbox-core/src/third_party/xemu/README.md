# xemu's MCPX audio processing unit

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
