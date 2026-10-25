# Building Crema from scratch

How the `Crema-v03.30.pkg` / `Crema-Lab-v03.30.pkg` of this fork were built on a fresh Ubuntu 24.04
machine (4 cores, ~25 GB of disk), without the DolphinPS4 build tree that
`scripts/seed-build-tree.sh` copies from. Everything comes from public sources at pinned versions.
About 1.5 hours, most of it the LLVM 12 linker.

## 1. Host tools

- clang / lld / llvm 21 from `https://apt.llvm.org/noble/ llvm-toolchain-noble-21` (clang 21.1.8):
  `clang-21 lld-21 llvm-21 libclang-rt-21-dev`
- `ninja-build cmake pkg-config glslang-tools python3-venv nasm`
- `~/cemups4-build/mesa-venv`: a venv with `meson mako pyyaml packaging`
- Go 1.17.5 (go.dev/dl) and the .NET 6 SDK (6.0.428), only to build the SDK tools below

## 2. The PS4 SDK: `/opt/pacbrew/ps4/openorbis`

PacBrew's `ps4-openorbis` recipes (github.com/PacBrew/ps4-openorbis), at their pinned versions:

| Piece | Source | Notes |
|---|---|---|
| `bin/ld.lld` | LLVM 12.0.1 release + PacBrew's `clang/001-ps4.patch` | only `lld` is built (`-DLLVM_ENABLE_PROJECTS=lld`); the patch is a compile fix |
| `lib/libc.a`, `crt1.o`, `crti.o`, `crtn.o`, C headers | PacBrew/ps4-openorbis-musl `140a653` | PacBrew's configure line, `CC=` clang 21 |
| `include/`, `samples/`, `link.x`, `lib/crtlib.o` | OpenOrbis/OpenOrbis-PS4-Toolchain `cd48023` | |
| `lib/*.so` stubs | OpenOrbis/orbis-lib-gen v1.3 release `lib.tar.gz`, + PacBrew's `libSceLibcInternal.so` | |
| `lib/libclang_rt.builtins-x86_64.a`, `lib/libunwind.a` | LLVM 12.0.1 compiler-rt / libunwind, PacBrew's flags | standalone builds need `-DLLVM_CONFIG_PATH=LLVM_CONFIG_PATH-NOTFOUND` |
| `bin/create-fself` | OpenOrbis/create-fself `30d2e10` | the version PacBrew ships; `check-oelf.cmake` guards its PT_LOAD bug (OpenOrbis master has the fix, `626d440`) |
| `bin/create-gp4` | OpenOrbis/create-gp4 `bf8e66e` | |
| `bin/linux/PkgTool.Core` | OpenOrbis/LibOrbisPkg `594021f`, `dotnet publish -r linux-x64 -c Release LibOrbisPkg.Core.sln` | + PacBrew's wrapper script |
| `cmake/ps4.cmake`, `ps4vars.sh`, `usr/bin/openorbis-*` | PacBrew `vars/`, `pkg-config/` | |
| `bin/clang`, `bin/llvm-*` | symlinks to the clang/llvm 21 tools | the builds below use clang 21 throughout |
| `usr/lib/libturbojpeg.a` | libjpeg-turbo 2.1.2, PacBrew portlibs recipe (`openorbis-cmake`) | clang 21 needs `-Wno-error=implicit-function-declaration` (clang 12 only warned) |

## 3. Sysroot, Mesa, Crema

With `PSB=PS4_BUILD_ROOT=~/cemups4-build`, `~/cemups4` a link to this repo, and the scripts
made executable (`git config core.fileMode false` keeps that out of git):

1. `toolchain/build-libcxx.sh` (libc++ 21.1.6 into the sysroot), `toolchain/build-runtime.sh`,
   `scripts/install-overlay.sh`
2. `scripts/build-deps.sh`. Seven of its sources are GitHub `/archive/` tarballs; when those can't
   be downloaded, `git clone` the same tag or commit into `~/cemups4-build/src/deps/<name>` first
   (`fetch` skips a library whose folder exists): libpng v1.6.47, fmt 12.1.0, glm 1.0.1, rapidjson
   24b5e7a, glslang 15.1.0, libsmb2 v6.0.0, libnfs libnfs-8.0.0.
3. Mesa: upstream `mesa-26.0.8` (gitlab.freedesktop.org/mesa/mesa) + these patches, in this order
   (`git apply --exclude=src/amd/common/ac_ps4_drm.c`; `build-mesa.sh` copies
   `port/mesa/ac_ps4_drm.c` in): `mesa-ps4`, `mesa-ps4-aligned-vertex-fetch`,
   `mesa-ps4-vertex-fetch-switch`, `mesa-ps4-flush-all-draws`, `mesa-ps4-gs-ring-probe`. All five
   are needed: Crema's settings reference `ac_ps4_flush_all_draws`, `ac_ps4_gs_ring_min` and
   `ac_ps4_unaligned_vertex_fetch`. libdrm `libdrm-2.4.125` (headers only) in
   `~/cemups4-build/src/libdrm`. Then `scripts/build-mesa.sh setup`.
4. Cemu at `patches/BASE.txt`'s commit + `patches/cemu-ps4.patch`, with the ZArchive, imgui and
   Vulkan-Headers submodules, in `~/cemups4-build/src/Cemu`. Then
   `scripts/configure-cemu.sh -DCMAKE_HAVE_LIBC_PTHREAD=1` (a fresh configure's `find_package(Threads)`
   test program can't link: the `--wrap` flags point at Crema's runtime, which it doesn't have;
   the PS4's pthreads are in its libc), `scripts/build-cemu.sh`, `scripts/check-eboot.sh`.
5. `CEMU_PS4_VERSION=03.30 scripts/package-cemu.sh` and `CEMU_PS4_LAB=1 ...` for Crema Lab.

## Checks done on the result

- The binary is linked by LLD 12.0.1; `check-oelf`: one RW PT_LOAD; `check-eboot.sh`: libraries
  imported by name, every runtime wrap/override defined.
- Against the official `Crema-v03.30.pkg`: the same 309 files, all identical except `eboot.bin`
  (17 KB larger: this fork's changes) and Crema Lab's `version.txt`; `param.sfo` identical apart
  from the date.
