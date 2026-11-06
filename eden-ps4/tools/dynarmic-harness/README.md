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

## Persistent code cache (04.17)

`cachetest` runs a program, saves its code cache (`Jit::SaveCodeCache`), drops everything of that
run (its objects are allocated from an arena that is then unmapped, so a pointer the cache failed
to move faults instead of finding the same object again), loads the cache into a second run whose
objects are all elsewhere (the JIT, the callbacks, the monitor, the coprocessor), runs the program
again and prints what `test` prints. `twice`: that run's cache saved and loaded into a third;
`cp15`: a coprocessor like Eden's (fields read and written through pointers, CNTPCT through a
callback with its parent as the argument; `programs/cp15.s`); `CACHETEST_B=<other.bin>`: the last
run runs another program (its blocks must all be found stale). The code caches need a fixed
address, as on the PS4: build dynarmic with `-DDYNARMIC_RWX_CODE_ALLOCATOR` into its own build
directory and set `DYNARMIC_CODE_ADDRESS`:

```sh
DEPS=$HOME/eden-deps; EDEN_SRC=$HOME/eden cmake -S wrap -B build-pptc2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
    -DCMAKE_CXX_FLAGS="-isystem $DEPS/xbyak -isystem $DEPS/fmt/include -DFMT_HEADER_ONLY -DARCHITECTURE_x86_64 -DDYNARMIC_RWX_CODE_ALLOCATOR"
BUILD=build-pptc2 OPTS="... -DHAVE_DEAD_NZCV -DHAVE_DEAD_NZCV_ABI -DHAVE_FOLD -DHAVE_COLD" ./build.sh dynbench_p4
DYN_COLD=1 DYN_RSB=1 ... ./ctx.sh gen7.py dynbench_p4 1 100        # DTX_OPTS: fastmem, splitstack, twice
```

`ctx.sh` runs a generator's programs through `test` and `cachetest` and compares them (and counts
the blocks used from the cache). For A64 (04.18): `DYNBENCH=dynbench64.cpp ./build.sh dynbench64_p5`
(same build directory), `dynbench64 cachetest <prog> [fastmem|fastmem2|noexcl|nowalk] [twice]` (the
thread registers read and written through pointers into the callbacks object, as Eden's;
`programs/tpidr64.s`), `ctx64.sh gen8.py dynbench64_p5 1 100` (gen5, gen6, gen8). (04.22) `-DHAVE_A64_PORT`: the A64
options ported from A32, `DYN_FOLD=1 DYN_EXT=1 DYN_COLD=1`; `programs/c/build64.sh` builds `game.c`
for AArch64 (`dynbench64 image programs/c/game64.bin <frames> fastmem2`). (04.23) `-DHAVE_DEAD_NZCV64`:
`DYN_NZCV=1` (the flags look-ahead), `DYN_NZCVABI=1` (calls and returns end it; not for gen8, whose
random functions read the flags a call left); `steady64.sh <harness> [VAR=1...]` counts 20 steady
frames of `game64.bin` (callgrind, 30 minus 10); `cgjit64.py <dir>` and `cgblock.py <dir> <n>` show
what the translated code executed by kind and the n-th hottest block (04.24: `-DHAVE_RSB_PC`,
`DYN_RSBPC=1`, the return stack of return addresses; `-DHAVE_FOLLOW`, `DYN_FOLLOW=1`, BL and
forward B translated through; measure with Eden's `DYN_FWD=1 DYN_RSB=1 DYN_DISP=1 DYN_NOFRAME=1` too;
`blockstats64.py <dir>`: block entries and host instructions per entry; 04.25: `-DHAVE_ACQ`,
`DYN_ACQ=1` plain load-acquires, `DYN_DMB=1` LD/ST-only barriers elided; `fastmem2 nowalk` shows
the shared walk's cost with fastmem; 04.28: `-DHAVE_SUPER`, `DYN_SUPER=1` forward CBZ/TBZ as side
exits; `programs/c/unity.c` (IL2CPP-shaped) as `unity64.bin`; 04.29: `DYN_SUPERCOND=1` forward
B.cond as side exits too, `DYN_IRDUMP=1` prints each block's IR as it is compiled; 04.30: `-DHAVE_BFS`,
`DYN_BFS=1` SBFM/UBFM as shifts; `gen18.py` bitfield encodings and W results read as 64 bits;
04.73-04.74, `-DHAVE_PINS`: `DYN_PINS=1` loop register pins, `DYN_REUSE=1` stack range-check reuse,
`DYN_GUARD=1` guarded links (the harness's `PeekCode`), `DYN_LEAF=1` short leaf calls followed,
`DYN_RET=1` known returns followed) (callgrind `--dump-instr=yes`
into `<dir>/cg.out`, with `DUMP_JIT=<dir>/dump PERF_BUILDID_DIR=<dir>/perf`). `EDEN_SRC=<tree>` builds another Eden tree (a git worktree),
`BUILD=<dir>` into another build directory.

| Generator | Programs of |
|---|---|
| gen.py | loads and stores of several registers near page boundaries (shared walks) |
| gen2.py, gen3.py | mixed ALU, memory, LDM/STM/PUSH/POP/VLDM/VSTM, calls |
| gen4.py | A32 exclusives (LDREX/STREX), marked and unmapped pages |
| gen5.py | A64 exclusives |
| gen11.py | A64 flag setters right before B.cond, CBZ/CBNZ/TBZ/TBNZ on every bit, edge values (04.23) |
| gen12.py | A64 scalar floats: arithmetic, conversions, compares, loads and stores of S and D (04.23) |
| gen13.py | A64 flag setters followed by CSEL/CSINC/CSINV/CSNEG/CSET/CINC/FCSEL in the same block (04.24) |
| gen14.py | A64 calls and returns around FPCR changes (the return stack across mode changes) (04.24) |
| gen15.py | A64 load-acquires, store-releases, exclusive pairs and DMBs of every type, ordinary and marked pages (04.25) |
| gen16.py | A64 sign extensions: LDRSB/LDRSH/LDRSW, SXTB/SXTH/SXTW (again), SMULL/SMADDL, extended-register ADD/SUB (04.26) |
| gen17.py | A64 conditional branches of every kind and register to flag readers, setters on the other path (04.28) |
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
