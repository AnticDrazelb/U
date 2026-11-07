# Results on a PS4

## 01.00, retail PS4 under GoldHEN, 2026-11-07

Transcribed from a photo of the results screen (the top lines of the segment list scrolled off).

### Segments

| selector | descriptor |
|---|---|
| `0x1b` | 32-bit data, read/write, dpl 3, limit 0xffffffff |
| `0x33` | **32-bit code**, dpl 3, limit 0xffffffff |
| `0x3b` | 32-bit data, read/write, dpl 3, limit 0xffffffff (ss) |
| `0x43` | 64-bit code, dpl 3, limit 0xffffffff (cs) |

The PS4 keeps FreeBSD's 32-bit user code segment (`0x33`). **Xbox code runs natively on the PS4's
CPU, in 32-bit compatibility mode, with no recompiler.**

### CPU

Best of two runs, against the same kernels built natively for the PS4. Guest memory at the Xbox's
own addresses (image at `0x10000`, data from `0x1000000`). All checksums correct.

| kernel | native | Xbox code | | first run |
|---|---|---|---|---|
| int | 0.378 s | 0.436 s | x1.15 | 0.436 s |
| branchy | 0.170 s | 0.182 s | x1.07 | 0.182 s |
| calls | 0.038 s | 0.037 s | x0.99 | 0.037 s |
| memory | 0.363 s | 0.398 s | x1.10 | 0.420 s |
| copy | 0.297 s | 0.711 s | x2.39 | 0.794 s |
| sse | 0.239 s | 0.280 s | x1.17 | 0.280 s |
| x87 | 0.816 s | 1.230 s | x1.51 | 1.232 s |

Geometric mean x1.28. `copy` (`rep movsd` against the native build's vector moves) and `x87` (no
SSE2 on a Pentium III) are the cost of Pentium III code, not of emulation. The Switch's AArch64
code ran x5.3 through dynarmic on the same console (`switch-bench/RESULTS.md`).

What an emulator around it pays:

| | |
|---|---|
| a call from the emulator into Xbox code and back | 171 ns |
| a kernel call from Xbox code to host code and back | 122 ns |
| a hardware register access (fault, decode, emulate) | **7.30 µs** (1.5 µs on the desktop) |

Three cores at once, each against one core alone: int 100%, branchy 99%, memory 81% (93% on
average).

### Memory (16 KiB pages)

- Free below 4 GiB: 4080 MiB, `0x1000000-0xffffffff`. Something occupies part of the first 16 MiB.
- The Xbox's RAM at its own addresses (`0x10000-0x3ffffff`): **no**. The range is in use; the
  system offered `0x974000` instead, so something ends there. Most likely the bench's own eboot,
  loaded at `0x400000`.
- The executable and alias checks of 01.00 used the same range, so they failed for the same reason
  and say nothing about the memory itself. 01.01 repeats them at `0x04000000`.
- An alias of one 4 KiB page: no (direct memory maps in 16 KiB pages only).
- The hardware range `0xfd000000-0xffffffff` kept unmapped: yes.

### What this means

- **The CPU is solved.** Xbox code runs at native speed; a Jaguar core at 1.6 GHz is roughly
  twice a 733 MHz Pentium III. A high-level kernel costs about 0.1 µs a call.
- **Register accesses through faults cost 7.3 µs on the PS4.** A game writes the NV2A's push
  buffer pointer (`PUT`) once per batch of commands, hundreds of times a frame at most: about 1-4
  ms a frame. Faster ways to see `PUT` move (watch the register page only, or poll the push buffer
  from the GPU thread) are worth having.
- **The Xbox's low addresses collide with the emulator's own executable.** XBE images load at
  `0x10000` and big ones reach several MiB, past `0x400000`. Ways around it, once 01.01 shows what
  is there:
  - load the emulator itself higher, if the system lets an app's executable be placed elsewhere;
  - give the guest its own code and data segments with a base (in compatibility mode segment bases
    still apply), so guest address 0 can be anywhere free. That needs LDT entries, which the PS4
    may not allow an app to set.
