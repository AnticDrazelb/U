# Results on a PS4

Switch CPU Bench 01.03 on a retail PS4 under GoldHEN, 2026-10-27, transcribed from the results
screen. CPU: family 0x16 model 0x13 (Jaguar), 8 hardware threads: +SSE4.1 +AVX -AVX2 -FMA -BMI2
+F16C +AES.

## CPU

Times are the best of two runs; the slowdowns are against the same kernel built natively for the
PS4. "first run" is Accurate's first run, including recompiling the kernel. All checksums are
correct; Auto and Unsafe differ only on `fma`, as allowed: they unfuse the multiply-adds.

| kernel | native | Accurate | Auto | Unsafe | first run | Auto MIPS |
|---|---|---|---|---|---|---|
| int | 0.504 s | 2.225 s x4.42 | 2.224 s x4.42 | 2.225 s x4.42 | 2.225 s | 463 M |
| branchy | 0.584 s | 3.211 s x5.50 | 3.208 s x5.49 | 3.205 s x5.49 | 3.205 s | 240 M |
| calls | 0.355 s | 3.606 s x10.16 | 3.610 s x10.18 | 3.607 s x10.17 | 3.608 s | 212 M |
| memory | 0.677 s | 0.844 s x1.25 | 0.841 s x1.24 | 0.842 s x1.24 | 0.992 s | 40 M |
| copy | 0.298 s | 1.678 s x5.63 | 1.667 s x5.59 | 1.666 s x5.59 | 1.683 s | 158 M |
| float | 0.367 s | 4.564 s x12.45 | 4.583 s x12.50 | 4.157 s x11.34 | 4.589 s | 127 M |
| fma | 5.419 s | 30.792 s x5.68 | 0.550 s x0.10 | 0.569 s x0.10 | 30.820 s | 233 M |

Three cores at once (Auto), each core's speed against running alone: int 100%, branchy 100%,
memory 78%.

The app's summary line says "3.0x slower than native (Auto; Unsafe 3.0x), 172M guest
instructions/s per core, 3 cores at once 93% each". The 3.0x includes `fma`, where Auto's unfused
multiply-adds beat the native build's software `fma()` (the Jaguar has no FMA instruction).
Without it, the geometric mean is **x5.3**. Without the memory kernel, which waits on RAM as a
Switch would, the geometric mean of the Auto MIPS is **219 M** guest instructions per second per
core.

Reading:

- An A57 at 1020 MHz retires roughly 1.0-1.5 G instructions per second on code like this. So one
  PS4 core runs about **15-25% of a fully busy Switch core**, and three PS4 cores run three Switch
  cores side by side at that rate. Games that keep each Switch core less than about a fifth busy
  could run at full speed; heavier ones would run at a fraction of it, before any GPU emulation
  cost.
- Auto is essential on the PS4: exact fused multiply-adds (Accurate) are 56x slower than Auto's
  unfused ones. Unsafe adds little (10% on vector float).
- Compiling is cheap here: first runs match warm runs.
- The slowest cases, calls (x10) and vector float (x12.5), are where dynarmic would be tuned for
  the Jaguar first.

## Memory

Measured at the end of the run: the app heap (512 MiB), six recompiler cores' code caches
(8 MiB each) and their guest memory were still allocated.

| what | result |
|---|---|
| regular flexible memory free | 403 MiB |
| app heap (Crema's runtime) | 512 MiB of system flexible memory |
| system flexible memory still free | 64 MiB |
| Eden's page table, 1 GiB (39-bit address space) | **doesn't fit in either pool** |
| direct memory | 4.50 GiB in all, 4.47 GiB free |
| Switch DRAM, 4 GiB as one CPU buffer | yes, leaving 480 MiB of direct memory |
| executable memory, system flexible | 64 MiB more (runs code) |
| executable memory, direct | 0 MiB (direct memory can't be mapped executable) |

What an Eden port would have to change:

- **The page table.** A flat table for a 39-bit address space (1 GiB) has nowhere to go. It
  could be smaller (a 36-bit table is 128 MiB), multi-level, or taken from direct memory, which
  in turn means less than 4 GiB of emulated DRAM. Switch games get about 3.2 GiB, the rest is
  the system's.
- **Code caches.** Executable memory comes only from the system pool, and the app heap takes 512
  MiB of it. The heap should move to regular flexible or direct memory, and the code caches
  shrink from Eden's 3 x 512 MiB to tens of MiB each.
- **GPU memory.** With the full 4 GiB of DRAM mapped, 480 MiB of direct memory is left for the
  PS4 GPU's own copies of textures and render targets.
