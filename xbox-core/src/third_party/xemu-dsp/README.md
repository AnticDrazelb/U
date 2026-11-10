# xemu's DSP56300 interpreter

The MCPX APU's GP and EP DSPs run on this code, taken from xemu (https://github.com/xemu-project/xemu,
`hw/xbox/mcpx/apu/dsp/`: `dsp.c`, `dsp.h`, `dsp_c.c`, `dsp_dma.c`, `dsp_dma.h`, `dsp_dma_regs.h`,
`dsp_internal.h`, `debug.h`, and `interp/`), itself adapted from Hatari's DSP 56001 emulation. It is
under the GNU General Public License, version 2 or later; see the files' own headers and `COPYING` at
the top of xbox-core.

Changes for xbox-core:
- `dsp.c`: the C interpreter only (xemu's JIT needs its dsp56300 library); no xemu settings.
- `interp/dsp_cpu.c`: X, Y and P memory accesses bounds-checked (xbox-core builds with asserts off).
- `shim/`: what the code uses of QEMU's headers (`qemu/osdep.h`, `qemu/bswap.h`, `qemu/compiler.h`);
  `trace.h`: xemu's trace points compiled out.

The glue that connects the DSPs to the APU (register windows, reset and bootstrap, scratch and FIFO
DMA through the scatter-gather lists, the per-frame run) is in `src/apu.cpp`, following xemu's
`hw/xbox/mcpx/apu/dsp/gp_ep.c`.
