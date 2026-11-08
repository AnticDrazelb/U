# xbox-core: handoff notes

Written for whoever picks this up next: another Claude session, or a person. Read all of it
before changing anything. The state described here is branch `X` at the commit that added this
file (8 November 2026).

## 0. Starting a new session

Paste this into a new Claude Code session on the `anticdrazelb/u` repository:

> Read `xbox-core/HANDOFF.md` on branch `X` in full and follow it. Check out branch `X`, run
> `xbox-core/setup-env.sh shadps4` to set up what §2 says isn't in the repo, and build with
> `xbox-core/build.sh host` and `xbox-core/build.sh ps4`. Then carry on from §5, starting with the
> console log I'll attach.

## 1. The goal and the rules

**Goal:** run original Xbox games on a PS4, starting with the owner's *Halo: Combat Evolved (USA)*
`.xiso`. Xbox code runs natively in 32-bit compatibility mode, with a high-level emulation (HLE)
kernel answering its calls to `xboxkrnl.exe`.

**The owner** tests on a real PS4 (firmware 13.52, 4.6 GB direct memory) and sends back log files.
They have little time and limited usage, and they dislike back-and-forth. Do everything possible
on the desktop first. Each console test should answer as much as possible, and the logs must
capture everything.

**Rules:**
- Work on branch `X` of `anticdrazelb/u`. Commit and push with `git push -u origin X`.
- Never open a pull request unless asked.
- Commit messages end with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01VdsH7Doy12NUjCWoh5uY9K
  ```
  A different session uses its own session URL.
- No model IDs anywhere in the repo.
- Don't recreate or redistribute copyrighted games, and don't put game files in the repo.
- xemu (GPL) may be read for hardware semantics only. Don't copy its code.
- Don't impersonate brands, and don't fabricate results. Say what was and wasn't tested.
- After each change for the console:
  1. Build the package (`out/xbox-core.pkg`).
  2. Send it to the owner.
  3. Tell them exactly what to do (usually: launch Halo, then send `xbox-core-logs.txt`).
  4. Tell them what each possible log line will mean.

## 2. Where things are

| what | where |
|---|---|
| the emulator | `xbox-core/` (`src/` core, `ps4/` PS4 main and platform, `frontend/` library UI, `tests/`) |
| build script | `xbox-core/build.sh host` (desktop, into `~/xc-host`) and `xbox-core/build.sh ps4` (`out/xbox-core.pkg`, build in `~/xc-ps4`) |
| how it works | `xbox-core/README.md` (component table) |
| PS4 toolchain setup | `docs/BUILDING.md` (OpenOrbis SDK at `/opt/pacbrew/ps4/openorbis`, sysroot in `~/cemups4-build`) |
| shared PS4 runtime | `port/` (crash logger `ps4_crashlog.cpp`, sampler and stack dump `ps4_sampler.cpp`, `pthread_create` wrapper `ps4_runtime.cpp`), shared with the Switch-emulator port |
| the earlier feasibility bench | `xbox-bench/` (`RESULTS.md`: console measurements) |
| the New Eden Switch emulator | a separate project of the owner's. Its log-bundling design was copied here; ask the owner if you need it. |

**Not in the repo** (a fresh container must rebuild these): `xbox-core/setup-env.sh` does all of
it on Ubuntu 24.04 in about 15 minutes, plus about 30 for shadPS4 with `setup-env.sh shadps4`. It
works around a network that refuses apt.llvm.org, go.dev, Microsoft's .NET downloads,
googlesource.com and GitHub `/archive/` tarballs; its header lists how that differs from
`docs/BUILDING.md`.
- **nxdk:** `~/emu/nxdk`, built with its samples. `tests/run-nxdk-samples.sh` builds the samples. Test XBEs are built with `make` in each `tests/*-test`; the disc test also needs `tests/disc-test/make-image.sh`.
- **shadPS4:** `~/emu/shadPS4/build/shadps4`. It only checks that the app opens and writes its files. It can't run guest code, because it can't place memory at the Xbox's addresses.
- **NevolutionX homebrew:** `~/emu/homebrew/NevolutionX/NevolutionX.iso`.
- **Halo:** not available here. Only the owner has it.

**Build environment for test XBEs:**
```
export NXDK_DIR=~/emu/nxdk PATH="$HOME/emu/nxdk/bin:/opt/llvm21/bin:$PATH"
```

**Running on the desktop:**
```
~/xc-host/xbox-core <xbe|iso> --seconds N [--png out.png] [--pad a@6,down@10]
```
Run it from a scratch folder, because it creates `xbox-data/` there.

**Regression set** (must stay at 0 failures):
- `tests/kernel-test/bin/default.xbe`
- `tests/seh-test/bin/default.xbe`
- `tests/disc-test/disc-test.iso`
- the nxdk samples triangle, mesh, xaudio, sdl, usb and sdl_gamecontroller, whose pictures should match `tests/*.png` apart from frame counters
- NevolutionX, which reaches its language menu

Running with `XB_STRICT_SIGNAL=1` makes the app abort if `Log` is ever called inside the fault handler. Keep that at zero.

**shadPS4 smoke test:** Xvfb must be running on :99.
```
rm -rf ~/emu/run/game && cp -r ~/xc-ps4/stage ~/emu/run/game && rm -rf ~/emu/run/game/sce_module
cd ~/emu/run && DISPLAY=:99 timeout 25 ~/emu/shadPS4/build/shadps4 ~/emu/run/game/eboot.bin
```
Its files land in `~/emu/run/user/data/xbox-core/`. Its USB drive is `~/emu/run/user/mnt/usb0`.

## 3. What the console logs give you

On the console the app writes everything to `/data/xbox-core/`. It also copies a bundle to the top
of every USB stick, as `/usb0/xbox-core-logs.txt` (or under `/mnt/usb0`). The bundle is written:
- at start and when the library opens
- every 15 s while a game runs
- after each watchdog snapshot
- when a program ends, and on fatal errors and crashes

The live log `xbox-core-live-log.txt` sits beside it, and the previous run's copy is
`xbox-core-live-log-previous.txt`.

The bundle holds:
- system info, settings, the library with run history, and the USB drives
- every `.log` and `.txt` in the data folder, the last 8 MiB of each, with the time each was written
- crash addresses named from the symbol table in the package

The data folder's files:

| file | what |
|---|---|
| `log.txt` and `log-1.txt` … `log-9.txt` | this session and the 9 before it. Every line has a seconds timestamp. Saved to disk every 0.5 s and before each launch step. |
| `crash.log` | a host crash report, appended to. Each app start writes a marker line with the build date, so older builds' entries can be told apart (their addresses symbolize wrongly against the current build). |
| `hang.log` | the watchdog snapshot: no picture 10 s into a program, or 10 s after the picture stops, and again every 30 s |
| `stacks-at-launch.log` | every host thread's stack at launch |
| `selftest-step.txt` | the CPU self-test step a run died in (see §4) |
| `history.txt` | per-game run counts and results |
| `boot-trace*.log` | the runtime's early start-up |

What a run's log contains:
- **Launch steps:** `launch: …` and memory mapping.
- **CPU self-test:** the `cpu self-test:` lines.
- **The game:** the XBE's sections, title ID and XDK library versions.
- **Kernel calls:** the first 5000 are traced as `call <thread>> Name(args) from eip` when they start and `call <thread>< Name = result` when they end. Every NTSTATUS failure is also logged.
- **Files:** every file open, with the host path and status.
- **Raw disk:** every raw disk read and write (`disk:`).
- **Faults:** the first 300 guest faults, as `cpu: fault N in thread T: signal, eip, address, code bytes, registers`, each saved to disk before it's handled.
- **Gaps:** what isn't emulated (`not emulated (kind): what`, each kind logged once and counted).
- **Exits:** launch data on `HalReturnToFirmware`, which tells you if the game went to the dashboard and why.
- **Stats:** a line every 10 s with fps, draws, kernel calls (and the five busiest), GPU methods and threads.

State dumps go into `hang.log`, end-of-program reports and crash reports. They show:
- each guest thread's last 24 kernel calls, with arguments, caller and result
- each thread's last fault, and the stacks at its last kernel call and last fault (64 dwords each)
- the last 128 faults (time, thread, signal, eip, address, outcome, all registers, 16 code bytes)
- the GPU's last 256 methods

## 4. The PS4 crash investigation (the important part)

**Symptom:** every launch died on the console, the app vanishing to the PS4 home screen ("blue
screen"). The same programs run on the desktop.

Findings in order, each from a console log:

1. **Halo quit to the dashboard** (`HalReturnToFirmware(2)`) right after reading partition 0's
   cache table (sector 4, offset 0x800). Our `partition0.img` was empty, so the read hit end of
   file. **Fixed** (f89b940): the raw disk and its partitions opened as volumes read zeros where
   nothing was written, and use the retail partition layout (`kPartitions` in `files.cpp`). A
   cache partition (X:, Y:, Z:) that gets a `FATX` header written to it is emptied, as formatting
   does.

2. **Any fault in guest code killed the app, with nothing logged.** Guest code runs with `fs` at
   its KPCR (a 32-bit address). libkernel's `sigaction` installs libthr's wrapper, which reads its
   thread data through `fs:[0x10]` before calling the handler, so it crashed inside the wrapper.
   xbox-bench never saw this because its guest code ran with the host's `fs`.

   **Fix:** set the handlers with the kernel's own sigaction (`platform_ps4.cpp`), in this order:
   1. libkernel's `syscall()`. It isn't exported on the owner's firmware.
   2. libkernel's raw sigaction stub, found by scanning `libkernel_sys.sprx`'s readable code for `mov rax, 416; mov r10, rcx; syscall`. **This is the one that works on the owner's console** (stub at `0x8000035c0`).
   3. Last resort: put the host thread's pointer at `KPCR+0x10` so the wrapper survives.

   The handlers restore the host `fs` first (`HostFs`). Signals 12 (SIGSYS) and 30 (the stack
   dump's) go through `Trampoline`, which restores the host `fs` and then calls the previous
   handler directly. `CheckFaultHandler` re-installs ours if something replaced it.

3. **Faults handled, but any handler doing real work died.** The self-test steps showed:
   - a plain fault (`cli`/`sti`, skipped): works
   - a fault on a bigger signal stack (256 KiB from a `.bss` pool; a 1 MiB heap stack killed the app): works
   - a fault on a new thread: works
   - a fault whose handler writes a log line: **dies**

   Stack alignment was checked and ruled out: the kernel enters the handler at rsp ≡ 8 (mod 16),
   which is correct, and the entries in `platform_ps4.cpp` realign anyway. Conclusion: the C
   library can't be used inside a handler that the kernel calls directly.

   **Fix (f389db4):** the signal handler (`OnFault` in `cpu.cpp`) only saves the fault into the
   thread's KPCR (`FaultEip`, `FaultEflags`, `FaultRegs` 0x350, `FaultSignal`, `FaultAddress`) and
   redirects the context to `xcs_fault64` (`switch.S`), in 64-bit mode on the guest stack. That
   entry:
   1. switches to the emulator's `fs` and stack, the same way the kernel-call gate does
   2. calls `FaultResumed` → `HandleGuestFault`: register emulation, privileged instructions, SEH delivery, logging
   3. switches back to the guest `fs`, restores the registers from the KPCR, then `popfq` + `lretq` into 32-bit code

   A nonzero result bails out the way `xcs_bail64` does. Faults inside the switch's own code are
   still handled in the handler, where `Log` drops its lines (`tl_in_signal_handler`). d7e5e65
   added that guard.

**Status:** the package with these fixes has been sent but **not yet tested on the console**.
Expected: the self-test's "fault log" step passes, and Halo gets past the cache-table read (its
trace stopped at `call 5< NtReadFile = 0`, from `0x1581f`). Halo's next fault is unknown, and the
fault ring will show it.

**The CPU self-test** (`InitCpu`, `cpu.cpp`) runs in named steps: `fault`, `fault log`, `signal
stack`, `new thread`. Before each step it writes the step's name and the build date to
`/data/xbox-core/selftest-step.txt`. If a run dies in a step, the next run with the same build
leaves that step out, with what it switches on, and logs that it did. A new build tries every step
again. So a step that kills the app costs one launch.

## 5. What to do next

1. **Wait for the owner's `xbox-core-logs.txt`** from the current package, then read it in this order:
   1. the `cpu: fault handlers set with …` line
   2. the `cpu self-test:` lines (does "fault log" pass?)
   3. the `call` trace and `cpu: fault N …` lines up to where the run stops
   4. the newest `crash.log` entry (after its build marker) and `hang.log` (check its date)
   5. the last 10 sessions' logs, if earlier runs matter

2. **If "fault log" still dies:** the resumed path itself has a problem on the console. Suspects:
   - `xcs_fault64` running `slot_fs_host` on the guest stack (`leave64` does the same, and works)
   - `0x310` (the host rsp) being stale for that thread
   - `popfq` with the saved rflags

   Add more self-test steps to isolate it, rather than guessing.

3. **If Halo gets further:** fix whatever the fault ring, trace or "not emulated" notes show next.
   The likely areas:
   - XAPI formatting the cache partition: raw writes to partition 3–5, `IOCTL`s (`files: device control …` lines), `NtFsControlFile`
   - Halo's D3D start-up on the NV2A: GPU methods, register faults at `0xfd…`
   - DirectSound and the APU
   - the 6 kernel imports not implemented yet: `IoInvalidDeviceRequest`, `NtUserIoApcDispatcher`, `IoStartPacket`, `IofCompleteRequest`, `IoStartNextPacket`, `IoMarkIrpMustComplete`

4. **Performance on the console:** every hardware-register access now costs a signal, two context
   switches and two `amd64_set_fsbase` calls. xbox-bench measured 7.3 µs per access before this
   change. If games are slow, decode-and-emulate the common access patterns in the handler without
   the C library, or patch hot paths.

5. **The "high" build** (linked at 0x1000000000) crashed at open on the owner's console. It's
   low priority. Use the normal build.

6. **Optional leftovers:** NV2A gaps (W-buffering, bump maps, signed blends, line widths), and the
   APU's DSPs.

## 6. Things to know

- `build.sh ps4` copies `frontend/assets` into the package. It also makes `stage/symbols.txt` with llvm-nm and Python, which the log bundle uses to name crash addresses.
- The library scans the games folder `/data/xbox-core/games` and USB drives (`/mnt/usbN` or `/usbN`: their top level, `games/`, `xbox/`, `Xbox/`, `xbox-core/games/`). Images are recognised by content, so `.iso` and `.xiso` both work.
- Hold L1 while the app opens to skip `autostart.txt`.
- Raw-disk images live in `/data/xbox-core/hdd/` (`partition0.img`, `partitionN.img`), and the partitions' file systems are folders there (`e/`, `c/`, `x/`, `y/`, `z/`).
- Don't call `Log`, `malloc` or anything else from libc on a path the signal handler runs. Test with `XB_STRICT_SIGNAL=1`.
- `out/xbox-core.pkg` is a build output and isn't committed. Rebuild it with `xbox-core/build.sh ps4`.
- Declare any `extern thread_local` variable `constinit` (and define it that way). Otherwise clang emits a weak reference to its thread-local init function (`_ZTH…`), and `create-fself` stops the PS4 build with "missing module for symbol".
