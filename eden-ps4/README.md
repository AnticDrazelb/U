# Eden for PS4: stage 2, Eden's core booting a Switch program

Stage 1 ([switch-bench](../switch-bench/README.md), results in
[RESULTS.md](../switch-bench/RESULTS.md)) measured Eden's recompiler and the memory a PS4 can give
it. Stage 2 runs Eden's actual Switch core on the PS4: the kernel, the scheduler and the four
emulated CPU cores, the HLE services, the loader. It runs a Switch program that writes text, with no
graphics or audio yet.

The first program is `hello/`, a small Switch homebrew program built by hand (no libnx). It checks
what a game needs first and prints the results through the Switch kernel's debug output call
(`svcOutputDebugString`):

```
hello: a Switch program is running, on emulated core 0
hello: memory available to it: 3283 MiB
hello: heap: 32 MiB at 0x2100c00000, written and read back
hello: computed 0xaf5bb1d62401f407 in 14 ms
hello: slept 250 ms, the clock says 250 ms
hello: a thread on core 1 computed the same
hello: all checks passed, exiting
```

That is the output on a desktop (`build.sh host`), where it passed 30 runs in a row, each exiting
cleanly. The PS4 build prints the same in shadPS4 (below); it hasn't run on a console yet. The
checks:

- the kernel's memory info (`svcGetInfo`)
- a 32 MiB heap (`svcSetHeapSize`), every word written and read back through the recompiler's page
  table
- a loop recompiled from AArch64 and timed by the emulated system tick
- `svcSleepThread`, measured against that tick (the scheduler and the emulated clock)
- a second thread on emulated core 1 (`svcCreateThread`, `svcStartThread`,
  `svcWaitSynchronization`) that must compute the same result
- `svcExitProcess`

Around those lines the launcher shows Eden's own warnings and errors. A bare NRO always brings a
few: no RomFS, no program ID or title (it has no NACP), and controller mappings for the Joy-Con
driver, which this build leaves out with SDL. The launcher sets no Cemuhook (UDP) controller
servers, so that client opens no socket.

## The PS4 app

`out/EdenPS4.pkg` (title ID EDNP00001, version in `VERSION`). It opens on a start page:

- **X** boots the test program (`hello.nro`, in the package).
- **SQUARE** boots `/data/eden-ps4/boot.nro`, if there is one. A text-only program might work; one
  that draws won't, as there's no GPU yet.

Without a controller, the test program boots after a countdown. If the last run didn't finish, the
page first shows what that run left behind: the end of its output (`log.txt`), its crash report
(`crash.log`) and the last steps of its boot trace. With a controller connected, it then waits for
a button, so a photo of it can come first. Stack addresses are ELF addresses: symbolize them with
`llvm-symbolizer --obj=<that build's ELF> 0x...`. `build.sh ps4` keeps every build's ELF in
`~/eden-ps4-elves`.

The program's output and Eden's warnings and errors go on screen and to `/data/eden-ps4/log.txt`.
Eden's data folders (NAND, SD card, keys) are in `/data/eden-ps4/user`. Nothing is written outside
`/data/eden-ps4`.

## Testing in shadPS4

As for stage 1 (see [its README](../switch-bench/README.md#testing-in-shadps4) for building
shadPS4 and running it headless): the PKG's staging folder `~/eden-ps4-build/stage` without
`sce_module/`, run from a folder with a `user/` subfolder, so the app's `/data` is `user/data`.
shadPS4 can't show what the console does with memory pools or executable memory, but the boot
trace shows which pools the runtime and Eden got their memory from.

With 01.00 (4 runs, all the same): the start page counts down, `hello.nro` boots, every check
passes and the program exits. The boot trace shows the app heap taking 384 MiB of system flexible
memory and the four 32 MiB code caches coming from the same pool, which runs code there; the 4
GiB of emulated DRAM mapped from direct memory. At the end Eden reports that it couldn't write
to its network interrupt pipe (`network.cpp:201`, logged, not fatal), which may be shadPS4's
`pipe`. After a run that crashed, the start page showed its log, its compacted crash report and
its last steps, and waited for a button.

## What the PS4 port changes in Eden

The patches are in `patches/`, made with `git format-patch` against Eden commit `eb88a40`:

| patch | what |
|---|---|
| dynarmic: PS4 code memory, A64 page table sign extension | stage 1's fixes ([switch-bench README](../switch-bench/README.md)) |
| build: SDL3 optional | `ENABLE_SDL3=OFF` builds without it (no SDL on the PS4) |
| frontend hooks | a log callback, `EDEN_EXTRA_FRONTEND_DIR` to build an out-of-tree frontend, and `SetAppDirectory` (Eden's data folder) on every POSIX platform, not just Android |
| core_timing: stop the timer thread before waking it | an upstream shutdown hang, found here |
| page table sign bit 55 | stage 1's finding: Eden's 57 is cleared by its own mask |
| PS4 memory | below |
| PS4 build | compile fixes: the PS4's compiler defines `__FreeBSD__`, but it lacks some FreeBSD headers and calls |

The PS4 memory changes, from what stage 1 measured:

- **The Switch's DRAM** (4 GiB) comes from direct memory, the GPU's 4.5 GiB pool, in pieces mapped
  back to back into one reserved range (`common/orbis_memory.cpp`, used by
  `common/host_memory.cpp`). Flexible memory, behind `mmap`, has well under 1 GiB.
- **The page table** (1 GiB for a 39-bit address space) only reserves its address range. Its 16
  KiB pages get memory as Eden fills them in (`common/sparse_large_vector.*`), as Eden already does
  on Windows. A recompiled read of an empty part faults, and a signal handler gives it a page of
  zeros. A game maps a few GiB, so its page table uses a few MiB. The GPU's and the device memory
  manager's tables are sparse vectors too, and work the same way.
- **Code caches** are 32 MiB per emulated core instead of 512 MiB: the PS4 runs code only from its
  system memory pool. The app heap (Crema's runtime) is capped at 384 MiB of that pool so that the
  caches fit (`PS4_SYSTEM_HEAP_MAX_MB` in `port/ps4_runtime.cpp`; Crema keeps 512 MiB).
- **No fastmem**: the PS4 can't map the same memory twice for an app.
- Fiber stacks were already 512 KiB on the PS4 upstream (2 MiB elsewhere). Eden makes about 75
  fibers (one per emulated thread, most of them for HLE services).

Other PS4 choices, in the launcher and `ps4-eden.cmake`: Boost.Asio is built without kqueue (the
PS4 has no `sys/event.h`), thread names and priorities aren't set (not calls Crema makes on these
consoles), and FFmpeg is built without its Linux-only calls (below).

## Building

Needs:

- Eden at commit `eb88a40` with `patches/` applied (`git am patches/*.patch`)
- CMake 3.31 or newer (`CMAKE=/path/to/cmake` if it isn't the one on `PATH`)
- Crema's PS4 toolchain and sysroot (`docs/BUILDING.md`; `toolchain/build-sdk.sh` builds its
  section 2) for the PS4 build
- clang 21 and lld for the test program

```
eden-ps4/fetch-deps.sh                # Eden's dependencies, as git clones, into ~/eden-deps
eden-ps4/build-ffmpeg.sh              # FFmpeg for the PS4 (the PS4 build only)
EDEN_DIR=~/eden eden-ps4/build.sh host    # desktop: ~/eden-host/bin/eden-ps4 <program.nro>
EDEN_DIR=~/eden eden-ps4/build.sh ps4     # out/EdenPS4.pkg
eden-ps4/hello/build.sh               # hello.nro on its own
```

`fetch-deps.sh` clones what Eden's CPM would download as GitHub archives (blocked in some build
environments), at the versions Eden pins, and applies Eden's patches for them. Boost and the time
zone database are release downloads, which CPM fetches itself.

`build-ffmpeg.sh` builds Eden's pinned FFmpeg with the parts Eden uses (the H.264, VP8 and VP9
decoders, two filters, swscale), through Crema's PS4 compiler wrapper. It turns off what the PS4's
C library links but can't do: `sched_getaffinity`, `prctl` and musl's `pthread_setname_np` are
Linux system calls, which the PS4's FreeBSD kernel doesn't have.

`build.sh ps4` checks the system functions the app imports against Crema's when `CREMA_ELF` points
at a Crema build's ELF: the consoles run Crema's, so every import must be one of them (as stage 1
checks; see its README).

On the desktop, `EDEN_PS4_DATA` sets the data folder (default: a new temporary folder) and
`EDEN_PS4_TIMEOUT` how long a program may run (default 60 s).
