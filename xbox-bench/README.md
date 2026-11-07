# Xbox CPU Bench: can a PS4 run original Xbox code on its own CPU?

Stage 1 of a feasibility study for running original Xbox games on the PS4, done as
[switch-bench](../switch-bench/README.md) was done for the Switch: before porting any emulator,
measure on a console what decides whether a port is worth doing.

For the Switch, the question was how fast the Jaguar recompiles AArch64 code. The answer, x5
slower than native, puts a PS4 core at about a fifth of a Switch core. The original Xbox is a
different case. Its CPU is a 733 MHz Pentium III, an x86 like the PS4's Jaguar, so the Jaguar
may not need to translate anything. It can run 32-bit x86 code itself in **compatibility mode**,
through a 32-bit code segment. Cxbx-Reloaded runs Xbox games on Windows that way. If the PS4's
kernel provides that segment, Xbox code runs at native speed, about twice the Xbox's own CPU.
This app finds out:

1. **Segments.** Which code and data segments the system set up, read with `lar`/`lsl` (safe in
   user mode). We need a 32-bit code segment at privilege level 3 and a flat 32-bit `ss`.
2. **CPU.** Seven kernels built for a Pentium III (`guest/kernels.c`: `-march=pentium3
   -mfpmath=sse`, so floats use SSE and doubles the x87), linked at `0x00010000` where an XBE
   loads, run in 32-bit mode from memory below 4 GiB. The same kernels are built natively as the
   reference, and the checksums must match bit for bit.
3. **The emulator's costs.**
   - a call from the emulator into Xbox code and back;
   - a *kernel call*: Xbox code calls an `xboxkrnl.exe` export through a gate into 64-bit host
     code and back, which is what a high-level kernel (Cxbx-Reloaded's, hakuX's) costs per call;
   - a *hardware register access*: Xbox code touches the NV2A's registers at `0xFD000000`, which
     are kept unmapped. It faults, and the fault handler decodes the `mov`, emulates it and steps
     over it, as Cxbx-Reloaded does MMIO.
4. **Three cores at once**: the game's CPU, plus the emulator's GPU and audio threads next to it.
5. **Memory.** What an emulator needs below 4 GiB:
   - which address ranges are free;
   - the Xbox's 64 MiB at its own addresses, readable and writable, and executable (the game's
     code lives there);
   - the RAM seen again at `0x80000000` (the Xbox's contiguous-memory alias, where games put GPU
     data), and whether that alias can be executable;
   - an alias of a single 4 KiB page, the Xbox's page size (the PS4 maps in 16 KiB pages);
   - the hardware range `0xFD000000-0xFFFFFFFF` kept unmapped.

It is one app (`out/XboxCpuBench.pkg`, title ID XBBN00001; version in `VERSION`). A full run
takes about a minute. The screen, start page and logs are switch-bench's:

- the results stay on screen and go to `/data/xbox-bench/results.txt`;
- if it crashes, Crema's runtime writes `crash.log` and `boot-trace.log` there, and the next
  start shows them;
- every risky step (the first switch into 32-bit code, each kernel, each memory check) is in the
  boot trace before it runs;
- **X** runs everything, **SQUARE** only the CPU tests, **TRIANGLE** only the memory checks.

If the system has something at the Xbox's own addresses, the bench uses a second layout at
`0x40000000` (an image linked there), and says so.

## Results so far

Desktop (Linux, a 64-bit process; Linux's 32-bit code segment is `0x23`), full run:

| kernel | native | Xbox code | |
|---|---|---|---|
| int | 0.106 s | 0.109 s | x1.02 |
| branchy | 0.066 s | 0.060 s | x0.91 |
| calls | 0.009 s | 0.009 s | x1.05 |
| memory | 0.143 s | 0.154 s | x1.08 |
| copy | 0.043 s | 0.158 s | x3.67 |
| sse | 0.052 s | 0.050 s | x0.98 |
| x87 | 0.221 s | 0.353 s | x1.60 |

- All checksums are correct, the x87 kernel included.
- A call in: 133 ns. A kernel call: 136 ns. A hardware register access: 1.5 µs. Three cores at
  once: 81% each.
- All the memory checks pass.

`copy` and `x87` aren't overhead: the CPU runs the 32-bit code as it is. They are what code built
for a Pentium III costs next to code built for the host. The copy is `rep movsd`, not 16-byte
vector moves; the doubles go to the x87, as there is no SSE2. An emulator can't make them faster
without rewriting the game's code.

**Not yet run on a PS4.** The PS4 build has only been checked to compile against stand-ins for
the system headers; it hasn't been built with Crema's toolchain. What decides everything is the
first line after `segments:`. FreeBSD, which the PS4's kernel comes from, puts a 32-bit user
code segment at `0x33` (its 64-bit one is `0x43`). Whether Sony's kernel kept it is what this run
shows.

## Research: how the original Xbox is emulated elsewhere

| emulator | where | CPU | kernel | GPU (NV2A) | status |
|---|---|---|---|---|---|
| **xemu** (continues XQEMU) | PC (Windows, Linux, macOS) | QEMU's recompiler (TCG), a full PC around it | the real Xbox kernel: needs the MCPX boot ROM, a BIOS and a hard disk image | low level: the push buffer decoded and replayed through OpenGL or Vulkan; audio (MCPX APU) emulated | the most compatible: about 867 of 1025 tested titles |
| **Cxbx-Reloaded** (continues Cxbx, Dxbx) | Windows | **native**: the XBE is loaded at its own addresses in a 32-bit process and runs on the host CPU | its own implementation of the kernel's exports | Direct3D 8 calls patched (HLE) onto Direct3D 9; an LLE path from XQEMU exists but is too slow | about 166 of 1032 playable |
| **X1 BOX** | Android | xemu port: QEMU's TCG, x86 to ARM | real kernel | xemu's | paid; the first on Android |
| **hakuX** (rfandango; hakuX CE fork) | Android (arm64, Snapdragon 845 or newer) | a JIT from x86 to ARM (its README: a FEX fork) | **its own reimplementation**: no BIOS, MCPX ROM or disk image needed (rfandango's line) | push buffer decoded into Vulkan; an *ubershader* interprets the register combiners (no shader compile per program), a vertex ubershader, pipelines prebuilt from earlier plays | about 97 titles playable (CE 0.5.2, October 2026) |
| **Xanite** | Android | new | | | an early proof of concept |

What each teaches for the PS4:

- **CPU: Cxbx-Reloaded's way, not a recompiler.** Every Android emulator has to translate x86 to
  ARM, and xemu translates even on x86. The PS4 can do what Cxbx-Reloaded does: load the XBE at
  its own addresses and run it. This bench checks that the PS4 allows it.
- **Kernel: high level, as hakuX and Cxbx-Reloaded do it.** The real kernel runs in ring 0, sets
  up its own page tables and uses privileged instructions; none of that runs in a PS4 app. Games
  call the kernel through the XBE's thunk table (exports by ordinal), so the loader points each
  entry at a gate into host code (the bench's kernel call). hakuX also shows which calls are worth
  answering in guest code without leaving it: the IRQL and time exports.
- **GPU: xemu's NV2A emulation with its Vulkan renderer, on Crema's Vulkan driver (RADV).** The
  push buffer is the one interface every game uses, so emulating it, not patching Direct3D,
  works for every title. hakuX's lessons:
  - the combiner ubershader, which took pipeline creation from about 400 ms per shader to none;
  - pipeline caches prebuilt in the background;
  - surface watches narrowed to the bytes written;
  - occlusion queries answered from the previous frame.
- **Memory: the Xbox has one memory for CPU and GPU, as the PS4 does.** Crema's Direct Vertex
  Data already lets the PS4's GPU read the Wii U's memory where it is (`docs/performance-notes.md`).
  For the Xbox that fits better still: textures, vertex and push buffers all live in the same 64
  MiB, at addresses the GPU gets as physical addresses (`addr & 0x0FFFFFFF`).

## The plan after this bench

If the 32-bit segment is there:

1. **Loader and kernel.**
   - An XBE loader at `0x00010000`, and the thunk table pointed at gates.
   - A high-level kernel: memory, threads (host threads, each running guest code), files (the
     XISO), events and timers.
   - The `fs` segment that Xbox code reads its thread through (`fs:[0x28]`): through an LDT entry
     if the system lets us set one, or by rewriting `fs:` accesses when the code is loaded.
2. **GPU.**
   - xemu's NV2A (`hw/xbox/nv2a`) without QEMU's device model.
   - Push buffer `PUT` writes caught as register accesses, or found by watching the buffer.
   - The Vulkan renderer on Crema's RADV.
3. **Audio**: xemu's MCPX APU, with its DSP processing off by default as hakuX does.
4. **Launcher**: Crema's (cover flow, settings per game).

If it isn't, the fallback is a recompiler from 32-bit to 64-bit x86. That is far simpler than x86
to ARM, because most instructions copy across as they are. Only memory operands, stack operations
and branches need rewriting.

Memory questions that decide the details (the memory checks answer them):

- Executable memory comes only from system flexible memory, and only 64 MiB of it was free in the
  Switch run, with Crema's 512 MiB heap in there. The Xbox's 64 MiB of RAM, where the game's code
  runs, needs that pool or a smaller heap.
- Direct memory can't run code (the Switch run). If it is also the only memory that maps twice
  (for the `0x80000000` alias; the bench checks), RAM that is both executable and aliased doesn't
  exist. The kernel would then hand out contiguous memory without an alias and translate
  addresses for the GPU itself.
- Pages are 16 KiB, not 4 KiB, so write watches on textures are coarser.

## Building

Needs:

- the fmt headers (Crema's PS4 sysroot has them, `docs/BUILDING.md`; any fmt 10-11 for the host);
- clang and ld.lld for the 32-bit guest;
- for the PS4, Crema's toolchain.

```
HEADERS_DIR=~/fmt/include xbox-bench/build.sh host   # desktop check: ~/xb-host/xbox_bench
xbox-bench/build.sh ps4                              # out/XboxCpuBench.pkg
```

Host knobs:

- `BENCH_SCALE=0.2` makes a quick run;
- `BENCH_MODE=cpu|memory` runs part of it;
- `BENCH_PROGRESS=1` prints each step to stderr.

The PS4 build reuses Crema's runtime (heap, mmap and thread wraps, crash log) with its log folder
moved to `/data/xbox-bench`, and switch-bench's stand-ins for the emulator hooks the crash log
calls (`switch-bench/src/ps4_stubs.cpp`). It should import only system functions switch-bench
already imports, but that hasn't been checked against a real build yet. To check:

```
llvm-nm -u <Crema ELF> | awk '{print $2}' | sort -u > crema.txt
llvm-nm -u ~/xb-ps4/xbox_bench | awk '{print $2}' | sort -u | comm -13 crema.txt -
```

The app icon is switch-bench's for now.

## Sources

- xemu: xemu.app, github.com/xemu-project/xemu; Emulation General Wiki, "Xbox emulators"
- Cxbx-Reloaded: github.com/Cxbx-Reloaded/Cxbx-Reloaded; Emulation General Wiki, "Cxbx-Reloaded"
- hakuX: github.com/rfandango/hakuX (README, HANDOFF.md), github.com/jreinach-alt/hakuX
  (docs/HISTORY.md, STATUS.md, release notes 0.5.2)
- Android coverage: Android Authority (hakuX, xemu coming to Android), Windows Central
