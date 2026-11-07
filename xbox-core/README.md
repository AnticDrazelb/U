# xbox-core: original Xbox programs on the host's own CPU

The emulator core that [xbox-bench](../xbox-bench/README.md) showed is possible on the PS4. Xbox
programs (XBEs) run **natively in 32-bit compatibility mode**: no recompiler, no BIOS, no Xbox
kernel. A high-level kernel answers the program's calls to `xboxkrnl.exe` on the host.
It runs on the desktop (Linux x86-64) today; the PS4 port is next.

**Where it stands:** nxdk's samples run, the C runtime, C++, the Windows API layer and SDL2
included. Their text and software-drawn pictures come out in the framebuffer, saved as PNGs.
Nothing that draws through the GPU (the NV2A) is emulated yet.

![nxdk samples in xbox-core](tests/samples.png)

*Top: `hello++` (C++), `winapi_filefind`. Middle: `winapi_drivelist`, `sdl` (SDL2 sprites).
Bottom: `gamma`, `triangle` (black: it draws with the NV2A).*

## How it works

| part | file | what |
|---|---|---|
| CPU | `switch.S`, `cpu.cpp` | far return into the system's 32-bit code segment; back through a far return to 64-bit code. `fs` is the guest's KPCR while Xbox code runs (`wrfsbase` on Linux, `amd64_set_fsbase` on the PS4), the emulator's own otherwise |
| kernel calls | `switch.S`, `kernel.cpp` | every import of the XBE's kernel thunk table points at a stub (`mov eax, ordinal; jmp gate`); the gate switches to 64-bit code and calls the handler with the guest's arguments, then returns with the export's stdcall/fastcall stack cleanup (`exports.inc`, from nxdk's `xboxkrnl.exe.def`) |
| threads | `cpu.cpp`, `kernel.cpp` | one host thread per guest thread; KPCR, ETHREAD and stack in guest memory, TLS laid out as the Xbox kernel does (`[StackBase + _tls_index*4]`) |
| objects | `kernel.cpp` | events, semaphores, mutants, threads, critical sections in the Xbox's own layouts in guest memory; waits on a host condition variable |
| memory | `memory.cpp` | RAM at `0x10000`, contiguous memory at `0x80000000` (separate: the PS4 can't alias), kernel area at `0x8c000000`, stacks at `0xd0000000`, devices from `0xfd000000` unmapped |
| devices | `devices.cpp`, `cpu.cpp` | an access to device memory faults; the fault handler decodes the instruction (`mov`, `movzx`, ALU ops, `test`) and does it on the NV2A's register file. vblank interrupt with its DPCs at 60 Hz |
| files | `files.cpp` | `D:` and `\Device\CdRom0` are the XBE's folder; the hard disk's partitions are folders under `xbox-data/` |

Privileged instructions that Xbox code may run are skipped or answered: `cli`/`sti`, `wbinvd`, port
I/O, `rdmsr`. `rdmsr 0x2a` reports a 5.5 × 133 MHz Pentium III, which nxdk's runtime reads.
A program's `hlt` ends that thread with a report of its caller's arguments: an assertion's text,
for example.

## Running it

```
cmake -S xbox-core -B ~/xc-host -G Ninja && ninja -C ~/xc-host
~/xc-host/xbox-core path/to/default.xbe --seconds 5 --png screen.png [-v 2]
```

`-v 2` logs every kernel call. Linux with FSGSBASE (kernel 5.9 or newer) is needed.

nxdk's samples are the tests: `NXDK_DIR=~/emu/nxdk tests/run-nxdk-samples.sh hello sdl gamma ...`
builds and runs them, with a PNG and a log for each in `tests/out/`.

## Next

1. **PS4.** The platform parts as xbox-bench has them:
   - `amd64_set_fsbase` for `fs` (found with `sceKernelDlsym`);
   - FreeBSD's signal context;
   - the PS4's memory calls;
   - the framebuffer on VideoOut.

   The XBE must stay below the app's own executable at `0x400000`
   (`xbox-bench/RESULTS.md`), or the executable moves (xbox-bench 01.04's variants).
2. **The NV2A.** xemu's NV2A emulation and its Vulkan renderer, already ported to the PS4's RADV in
   xemu-ps4. Push-buffer writes would be caught on the register page.
3. More of the kernel as programs need it: directory listing (`NtQueryDirectoryFile`), the `\??`
   object directory, timers, APCs, the XISO file system.
