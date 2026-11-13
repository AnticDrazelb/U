# xbox-core: original Xbox programs on the host's own CPU

> Picking up the work? Read [HANDOFF.md](HANDOFF.md) first: the current state, the PS4 crash
> investigation, how to build and test, and what to do next.

The emulator core that [xbox-bench](../xbox-bench/README.md) showed is possible on the PS4. Xbox
programs (XBEs) run **natively in 32-bit compatibility mode**: no recompiler, no BIOS, no Xbox
kernel. A high-level kernel answers the program's calls to `xboxkrnl.exe` on the host.
It runs on the desktop (Linux x86-64) and builds for the PS4.

**Where it stands:** nxdk's samples run: the C runtime, C++, the Windows API layer, SDL2, the
NV2A's 3D drawing (vertex programs, textures, register combiners) in a multi-threaded software
renderer, sound through the AC'97 and the APU's voice processor (DirectSound's hardware), and the Xbox
controller over USB. Games load from a folder or
straight from a disc image (`.iso`). 280 of the kernel's 337 functions are implemented (most of the rest are the driver model's IRPs and device queues), each new
one checked by `tests/kernel-test`. No commercial game has been tried yet.

![nxdk samples in xbox-core](tests/samples.png)

*Top: `hello++` (C++), `winapi_filefind`. Middle: `winapi_drivelist`, `sdl` (SDL2 sprites).
Bottom: `gamma`, `triangle` (black: it draws with the NV2A).*

## How it works

| part | file | what |
|---|---|---|
| CPU | `switch.S`, `cpu.cpp` | far return into the system's 32-bit code segment; back through a far return to 64-bit code. `fs` is the guest's KPCR while Xbox code runs (`wrfsbase` on Linux, `amd64_set_fsbase` on the PS4), the emulator's own otherwise |
| kernel calls | `switch.S`, `kernel.cpp` | every import of the XBE's kernel thunk table points at a stub (`mov eax, ordinal; jmp gate`); the gate switches to 64-bit code and calls the handler with the guest's arguments, then returns with the export's stdcall/fastcall stack cleanup (`exports.inc`, from nxdk's `xboxkrnl.exe.def`) |
| threads | `cpu.cpp`, `kernel.cpp` | one host thread per guest thread; KPCR, ETHREAD and stack in guest memory, TLS laid out as the Xbox kernel does (`[StackBase + _tls_index*4]`) |
| objects | `kernel.cpp` | events, semaphores, mutants, timers, threads, critical sections, read/write locks in the Xbox's own layouts in guest memory; waits on a host condition variable; APCs run in alertable waits; timers fire from their own thread (DPCs, APCs, periods) |
| library | `kernel_rtl.cpp` | the Rtl string, memory, integer and printf functions, the pool, interlocked lists, HMAC-SHA1 and RC4 |
| library | `kernel_crypto.cpp` | DES and triple DES (ECB, CBC), RSA over the console's key blobs, PKCS #1 signature checks |
| memory | `memory.cpp` | RAM at `0x10000`, contiguous memory at `0x80000000` (separate: the PS4 can't alias), kernel area at `0x8c000000`, stacks at `0xd0000000`, devices from `0xfd000000` unmapped |
| devices | `devices.cpp`, `cpu.cpp` | an access to device memory faults; the fault handler decodes the instruction (`mov`, `movzx`, ALU ops, `test`) and does it on the device's registers. vblank interrupt with its DPCs at 60 Hz |
| GPU | `nv2a.cpp`, `render.cpp`, `draw.cpp`, `pixel.cpp` | the NV2A's FIFO (DMA pusher, RAMHT, objects, interrupts, flips) and its 3D engine in software: vertex programs interpreted, triangles rasterized by a pool of threads (bands of rows, in order), textures (swizzled, linear, DXT, palettes), the register combiners, alpha test and blending |
| sound | `audio.cpp`, `apu.cpp` | the AC'97's bus masters played at 48 kHz (PS4: AudioOut; desktop: `--wav`); the MCPX APU that DirectSound drives: its front end (voice methods, lists, SSL streams, SGE pages, notifiers) and voice processor (envelopes, PCM and IMA ADPCM, pitch, 32 mix bins); its DSPs aren't run, the voices' own mix is heard |
| input | `usb.cpp` | the OHCI host controller and an Xbox controller on the port games see as player 1 (PS4: the DualShock 4; desktop: `--pad`) |
| files | `files.cpp`, `xiso.cpp` | `D:` and `\Device\CdRom0` are the XBE's folder or its disc image (XDVDFS, also inside full dumps); the hard disk's partitions are folders under `xbox-data/`; names match regardless of case |

Privileged instructions that Xbox code may run are skipped or answered: `cli`/`sti`, `wbinvd`, port
I/O, `rdmsr`. `rdmsr 0x2a` reports a 5.5 × 133 MHz Pentium III, which nxdk's runtime reads.
A program's `hlt` ends that thread with a report of its caller's arguments: an assertion's text,
for example.

## Running it

```
cmake -S xbox-core -B ~/xc-host -G Ninja && ninja -C ~/xc-host
~/xc-host/xbox-core path/to/default.xbe --seconds 5 --png screen.png [-v 2]
~/xc-host/xbox-core game.iso --wav sound.wav --pad a,start,lx=-20000
```

`-v 1` reports drawing time, `-v 2` logs every kernel call. `--pad` holds controller input from the
first second on. Linux with FSGSBASE (kernel 5.9 or newer) is needed.

Tests:
- nxdk's samples: `NXDK_DIR=~/emu/nxdk tests/run-nxdk-samples.sh hello sdl gamma mesh ...` builds
  and runs them, with a PNG and a log for each in `tests/out/`.
- `tests/kernel-test`: kernel exports against known results (`make`, then run `bin/default.xbe`;
  each check is a `PASS`/`FAIL` line in the log).
- `tests/disc-test`: file access on D:, from a folder and from a disc image (`make`, then
  `./make-image.sh`; run `bin/default.xbe` and `disc-test.iso`).

## On the PS4

`build.sh ps4` makes `out/xbox-core.pkg` (title ID XBCR00001) with nxdk's samples and the two
tests inside.

- The app opens on a menu: the samples, and your own games as
  `/data/xbox-core/games/<name>/default.xbe` (that folder is D:) or `/data/xbox-core/games/<name>.iso`.
  UP/DOWN choose, X runs.
- `/data/xbox-core/autostart.txt` holding an XBE's or image's path skips the menu.
- The program's screen is scaled up on the TV, with the log over it for the first 15 seconds
  (L3 and R3 together toggle it).
- The DualShock 4 is the Xbox controller: Cross/Circle/Square/Triangle are A/B/X/Y, L1/R1
  white/black, L2/R2 the triggers, OPTIONS start, the touch pad's click back. Rumble reaches it.
- The log is also in `/data/xbox-core/log.txt`, and crash reports in `/data/xbox-core/crash.log`.
- To run another program, close the app and open it again. When a game ends, or stops with an
  error it didn't handle, a line at the top of the screen says so (and the error), over its last
  picture, with the log's last lines.
- A game whose first half minute in xemu's renderer ends three starts in a row (the console stopping
  the app, or the app closed then) starts once with Midway's own NV2A, slower. That count is the
  game's own: another game's starts don't change it, and a game that ends or stops by itself
  doesn't add to it.

Its first log lines check the hardware path:
- `cpu self-test: 32-bit code runs, its fs is its KPCR (...)` means the switch into 32-bit code,
  the guest's `fs` (through `amd64_set_fsbase`) and the way back all work;
- `memory: 0x400000-... in the RAM is the system's; left out` is the app's own executable, which the
  RAM is mapped around. Programs whose image reaches past `0x400000` won't load until the
  executable moves (xbox-bench 01.04).

**Not run on a PS4 yet.** In shadPS4 the app itself works (menu, autostart, screen, log), but the
core stops at its first mapping: shadPS4's address space starts at `0x400000`, so it can't give the
Xbox's addresses below it. The real console could (xbox-bench).

## Next

1. **A console run**: the self-test line, then `sample: test-kernel`, `sample: test-disc (disc
   image)`, `sample: mesh`.
2. **The app's executable out of the Xbox's addresses**, for games whose image is bigger than the
   ~4 MiB below `0x400000`. xbox-bench 01.04's variants show which way the system allows.
3. **Commercial games**: the XDK's own drivers bring the APU (DirectSound), more of the NV2A
   (points, lines, cube maps, bump mapping, fog), structured exception handling, and the rest of the
   kernel's I/O calls.
4. **Drawing on the PS4's GPU** instead of the CPU, once games show what they need.

## The PS4 HOME menu

The app opens on a dashboard in black and white: Dark (white on black) or Light (black on white).
The theme is only the menu's own colours: covers, the games' art and your icon keep theirs.

Along the top are Midway's name, the time, a USB stick when a drive is plugged in, your name and
your icon. Under them are the tabs **Games**, **Library**, **USB** and **Settings**; L1/R1 move
between them, or Up to the tabs and then Left/Right. A dot on USB means a drive holds games that
aren't on the PS4 yet.

- **Games** is a row of covers. The selected one stands larger, with its name, what it is and how
  to start it beside it. Under the row, a panel shows what Midway knows of the game: when you last
  played it, how often, how its last run ended, its facts, and the last picture it showed (from
  your last session). The row ends in *All games*, which opens the Library.
- **Library** is every title in a grid.
- **USB** shows what the drives hold.
- **Settings** has these sections:
  - the theme;
  - graphics (the GPU's emulation, the picture's scaling, the loading log);
  - sound effects;
  - the title order, the logs (Off: nothing written while a game runs, no profiler, no
    pictures; crashes are still recorded), hidden titles, and Close Midway;
  - your nickname and icon;
  - the controls;
  - what's new;
  - about.

The games are the samples the app carries, your games in `/data/xbox-core/games` (a folder with
`default.xbe` and its files, or a disc image `<name>.iso`), and the games on USB drives (at a
drive's top, or in `games/`, `xbox/` or `xbox-core/games/`). Each shows its own title, title ID
and region (read from its XBE), its size, and when it last ran and how that ended.

Cover art is yours to add: `cover.jpg`/`cover.png` in a game's folder, `<name>.jpg` beside an
image, or `/data/xbox-core/covers/<title ID>.jpg`. A cover is made a square tile: the whole cover
on a blurred copy of itself. Without one, a tile is drawn from the title.

OPTIONS on a game opens its details and these choices:
- **Box Art** imports a picture as the game's cover. It lists the `.jpg` and `.png` pictures on
  the USB drives (at a drive's top and in the folders under it, two deep, such as `covers/` or
  `boxart/`), those whose name has the game's title, title ID or file name first, marked
  **Match**. Cross copies the chosen one to `/data/xbox-core/covers/imported/` (named by the
  title ID), where it comes before any other cover, so the drive can go afterwards. Square
  removes the imported one, and the game's own cover comes back.
- **Copy to the PS4**, for a game on a USB drive. The copy runs in the background, with its
  progress shown.
- **Start at Launch**, for any other game. Hold L1 while the app opens to get the menu back.
- **Delete**, for your games, on the PS4 or on a USB drive. It asks first and says what goes: the
  game's folder and all that's in it, or its disc image (and a cover beside it). It only deletes
  inside `/data/xbox-core/games` or on a USB drive, and the game's saves on Midway's Xbox hard
  disk stay.
- **Remove**, for the samples in Midway's package, which can't be deleted: it takes them off the
  list (`/data/xbox-core/hidden.txt`). Settings, System, Hidden Titles puts them back.

On the **USB** tab, Square copies every USB game that isn't on the PS4 yet, and Triangle writes
every log to `xbox-core-logs.txt` on the drives.

When you choose a game, a loading screen shows its tile until its first picture, with the log's
last lines under it. If it stops with an error, the whole log is shown instead.

The screen is drawn in software (`frontend/home.cpp`, `frontend/ui/`). The canvas, fonts, dialogs and
keyboard were begun from New Eden's menu. The fonts are Inter and Pixelify Sans, both under the SIL
OFL; they are built into the program, and their licences are in the package's `licenses/`. `tools/ui-preview.sh` (after `build.sh host`) draws every screen to PNG
files from a made-up library.

## Logs and diagnostics

Everything goes into one file at the top of each USB drive, `xbox-core-logs.txt` (as New Eden's
`eden-ps4-logs.txt`): every `.log` and `.txt` in `/data/xbox-core` (the last 8 MiB of each), code
addresses named from the package's `symbols.txt`. It's written as the app starts, when the
library opens, every minute while a program runs, when it ends, stops on an error or crashes;
`xbox-core-live-log.txt` beside it gets the log line by line. What the logs hold:

- every line timed (seconds since the start); each launch step before it's taken
- the program: its certificate, sections and the XDK libraries it was built with
- every file it opens (where that went, and how it ended; failures counted)
- what wasn't emulated, counted: kernel exports, GPU methods, device registers, files not found
- every guest exception (the first hundred with where)
- a line every 10 s: frames, picture changes, draws, kernel calls (the busiest), GPU methods,
  threads, free memory
- `hang.log` when frames stop for 20 s (or none come in 90 s): each guest thread's kernel call and
  its latest 24 calls with their arguments, the GPU's latest 256 methods, the host's thread stacks
- `crash.log` on a crash: the host's registers and stack (named), the same guest state; a guest
  fault or a fatal error puts it in the log too; SIGSYS (a refused system call) is caught as well

## Licence

xbox-core is distributed under the GNU General Public License, version 2 or later (`COPYING`): its
audio processor is xemu's (`src/third_party/xemu`, GPL-2.0-or-later and LGPL-2.1-or-later), with
libsamplerate (`src/third_party/libsamplerate`, BSD-2-Clause).
