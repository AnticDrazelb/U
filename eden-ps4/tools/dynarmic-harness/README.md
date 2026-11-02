# dynarmic harness

dynarmic (from `~/eden/src/dynarmic`, as patched for the PS4) built on its own for the desktop,
running guest code with the PS4 port's configuration: Eden's packed page table entries with the
whole offset, shuffled host pages, a page marked for the slow path (`0x10800000`) and an unmapped
one (`0x10900000`), unsafe optimisations, and optionally fastmem, a split (unaligned) stack, and
the options Eden turns on (`DYN_*` environment variables, see `dynbench.cpp`). It's how the
translator changes since 03.79 were measured and checked before going to a console.

## Build

```sh
CMAKE=/path/to/cmake-3.31 ./build.sh dynbench20     # A32 harness as ./dynbench20
DYNBENCH=dynbench64.cpp ./build.sh dynbench64x       # A64 harness
```

`build.sh` configures `wrap/` (dynarmic alone) into `build/` once, rebuilds the library, links
the harness. Needs clang-21, ld.lld-21, llvm-objcopy-21 and `~/eden-deps` (fetch-deps.sh) for
fmt and xbyak (and Boost's headers: `libboost-dev`). Keep the previous build's binary under another
name: the tests compare two. Options added since 04.02 are compiled in with `OPTS` (link.sh's list
plus e.g. `-DHAVE_DEAD_NZCV`) and switched on at run time (`DYN_NZCV=1`).
`DYNARMIC_HOST_FEATURES_MASK=16b00f` limits the host features to the Jaguar's.

## Run

```sh
./asm.sh programs/calls.s rt/calls.bin               # ARM (asmt.sh: Thumb, asm64.sh: A64)
./dynbench20 bench rt/calls.bin 1000000 [fastmem] [splitstack] ...  # time it (r0 = iterations)
./dynbench20 test rt/calls.bin arm [options]         # registers, ext registers, memory hash
```

## Cache simulation (04.07)

`programs/c/big.py <functions> <seed>` writes game-like C with a large code footprint (every
function called once a frame through a table, as a game's update and render code is); built like
`game.c`, it runs in image mode. callgrind with Jaguar-like caches, at two frame counts (the
difference is the steady frames, without the translation):

```sh
valgrind --tool=callgrind --cache-sim=yes --I1=32768,2,64 --D1=32768,8,64 --LL=2097152,16,64 \
    ./dynbench_rsb image programs/c/big1500.bin 4 arm fastmem      # and 2; DYN_COLD=1: cold code apart
```

## Differential tests

Random programs from a generator, run on the old harness and the new one (and the new one
single-stepped), outputs compared:

```sh
./dtx.sh gen9.py dynbench19 dynbench20 1 300                 # A32 (ARM and Thumb)
DTX_OPTS=fastmem ./dtx.sh gen9.py dynbench19 dynbench20 301 500
./dtx64.sh gen8.py dynbench64n dynbench64w 1 200             # A64
./dtxa.sh gen10.py dynbench_base dynbench_nzcv 1 400         # ARM-only generators
./xh.sh gen7.py arm dynbench19 dynbench20 1 100 [options]    # same programs, two harnesses
./strict_sweep.sh 1 100                                      # xbyak's strict checks on (build-strict/)
```

Failing programs are kept as `rt/failx_*.s`.

| Generator | Programs of |
|---|---|
| gen.py | loads and stores of several registers near page boundaries (shared walks) |
| gen2.py, gen3.py | mixed ALU, memory, LDM/STM/PUSH/POP/VLDM/VSTM, calls |
| gen4.py | A32 exclusives (LDREX/STREX), marked and unmapped pages |
| gen5.py | A64 exclusives |
| gen6.py | A64 loads/stores sharing a base |
| gen7.py | A32/Thumb control flow and flags: conditional instructions, IT blocks, loops, calls, VFP compares |
| gen8.py | A64 control flow and flags |
| gen9.py | NEON VLD1/VST1 (every size, count, alignment, writeback), lanes, VLD2-4, VLDM, VLDR |
| gen10.py | ARM flags across blocks: setters (all flags, N and Z, the shifter's C) before branches, loops and calls; every kind of reader where they lead, functions that read their caller's flags (04.02's dead flags) |

`manyblocks.py <functions> <out.s>`: a large program (functions with compares and calls to leaves a
megabyte away), run once with `test`: its time is translation at a game's scale (04.02's look-ahead
ranges made it 18.3 s instead of 0.4 s for 3,000 functions; the small programs never showed it).

`programs/` holds the hand-written benchmarks (calls with PUSH/POP, NEON copies, atomics...);
`programs/flagloops.s` Mario Kart's hottest loops from 04.01's `hotblocks.log`;
`programs/c/game.c` game-like C for ARM (`programs/c/build.sh [-O...]`, then `image
programs/c/game.bin <frames> arm`); `programs/c/zbench.c` zlib's deflate and inflate
(`programs/c/build-zlib.sh [-O...]`, then `image programs/c/zbench.bin <rounds> arm`): real
compiler output, for changes that rely on the procedure call standard (`DYN_NZCV_ABI`, which
random programs that don't keep to it fail; `GEN_ABI=1 gen10.py` makes ones that do).

What the translated code executes, instruction by instruction: `valgrind --tool=callgrind
--dump-instr=yes --smc-check=all` with `DUMP_JIT` and `PERF_BUILDID_DIR` (each block's code and
address), the costs at the blocks' addresses, disassembled and grouped (04.05 chose its changes so).
`programs/wrap.s`: loads and stores whose base plus offset passes 4 GiB, and on the marked and
unmapped pages (`DYN_FOLD`). `DYN_EXT`: no redundant extensions.

Instruction counts (no hardware counters in a VM): `valgrind --tool=lackey --basic-counts=yes
--smc-check=all ./dynbench bench <bin> N` at two N, the difference over the iterations (bench mode
runs N twice). `DUMP_JIT=<dir> PERF_BUILDID_DIR=<dir>` dumps each block's code in `bench`, `image`
and `test` mode.
