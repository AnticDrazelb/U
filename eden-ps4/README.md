# Eden for PS4: stage 3, Switch games playable; stage 4 begun, the GPU

**Performance work: where it stands, what was tried and what's next is in [PROGRESS.md](PROGRESS.md).**

Stage 1 ([switch-bench](../switch-bench/README.md), results in
[RESULTS.md](../switch-bench/RESULTS.md)) measured Eden's recompiler and the memory a PS4 can give
it. Stage 2 ran Eden's actual Switch core on the PS4: the kernel, the scheduler and the four
emulated CPU cores, the HLE services, the loader, with a Switch program that writes text. Stage 3
plays a game: a Switch homebrew game's picture on the screen, the PS4 controller as the Switch's,
and its sound.

## Stage 3: games

`game/breakout.c` is a small Switch game, written for this: Breakout at 640x360, drawn by the CPU
into a libnx framebuffer (no GPU commands), with the controller read through HID and beeps played
through `audout`. It's built with libnx, the Switch homebrew library, compiled with clang (below).
In the package as `breakout.nro`, first on the start page's list.

`game/blocks.c` is a second game: falling blocks with a ghost piece, a next-piece preview, levels,
and a tune (Korobeiniki, a 19th-century Russian folk song) under the beeps. Its best score is kept
on the emulated SD card (`/data/eden-ps4/user/sdmc/blocks-best.txt` on the PS4), through Eden's
file system services. The two games share `game/nxgame.h` (drawing, the font, sound).

`game/padtest.c` is a controller test: every Switch button with the PS4 button it's mapped to, lit
while held, and the sticks' positions, as text through libnx's console (also drawn by the CPU).

![Blocks in shadPS4](screenshots/blocks-shadps4.png)
![The controller test in shadPS4, Cross held and the left stick right](screenshots/padtest-shadps4.png)

![Breakout in shadPS4](screenshots/breakout-shadps4.png)

What runs it:

- **Picture.** A Switch program shows a frame by queueing a buffer to the display service (VI),
  whose compositor (nvnflinger) hands its layers to the renderer. Eden's null renderer threw them
  away; patch 0008 has it compose them on the CPU instead, for a frontend that asks for frames
  (`EmuWindow::WantsSoftwareFrames`): each layer's buffer read from emulated memory, unswizzled from
  the GPU's block-linear layout, converted to RGBA (RGBA8, RGBX8, BGRA8 and RGB565), cropped,
  flipped and blended as the compositor asks. A frame of one layer, the usual case, goes to the
  launcher at its own size (Breakout's: 640x360), more layers scaled to 1280x720 and blended. The
  launcher scales it to 1920x1080 into the video-out buffers, a row at a time from a row buffer:
  those buffers are write-combined memory, which the CPU reads uncached (shadPS4 backs them with
  ordinary memory, so it wouldn't show the cost of reading them). A game that draws with the GPU
  (NVN, Vulkan, OpenGL) shows nothing: its buffers hold whatever the GPU would have drawn, and
  there's no GPU emulation.
- **Controller.** The PS4 pad, read every 4 ms, is player 1's Pro Controller, through Eden's virtual
  gamepad input engine (`Launcher::SetPad`). By the PS4's conventions: Cross is A, Circle B, Square
  Y, Triangle X; L1/R1 L/R, L2/R2 ZL/ZR, OPTIONS +, the touchpad's click -, and both sticks.
- **Sound.** Patch 0009 adds a frontend sink to Eden's audio (`AudioEngine::Frontend`): each audio
  stream a program opens (`audout`, `audren`) plays through an output the frontend gives it, from a
  thread of its own, in 512-frame buffers. On the PS4 that's an AudioOut port (the system user's
  main port, 48 kHz stereo, as Crema's audio), on a thread raised above the emulation's priority.
  On the desktop, `EDEN_PS4_AUDIO=<file.wav>` records it. If an output can't open, its stream plays
  silently at an output's pace, so the program doesn't wait on it. Both of libnx's ways to play
  sound work: `audout` (Breakout) and the audio renderer `audren` (switchbrew's audren-simple
  example, recorded on the desktop).

In shadPS4, Breakout runs at 26 to 30 frames per second (61 on the desktop build): the start page,
X, the game; the pad's X launches the ball, the stick moves the paddle, OPTIONS quits back to the
start page. That rate is shadPS4's display here, which draws with a software Vulkan device: each
frame waits 28 to 30 ms for a buffer to come off the screen, and scaling it onto the screen takes
2.2 to 2.4 ms (the boot trace's numbers). A console flips in hardware. It hasn't run on one yet.

Most homebrew programs built with libnx ask for all the memory there is as their heap at start,
about 3.2 GiB, which Eden's kernel clears. On the desktop that took about 30 s, every page of the
emulated DRAM faulting in to be written; patch 0011 drops the pages instead, and they read as zeros
(Linux only). On the PS4 the emulated DRAM is direct memory, committed from the start, so clearing
it is a plain write of 3.2 GiB, a second or so; the package's games and controller test ask for
64 MiB anyway.

## Stage 2: the test program

Stage 2's program, still in the package, is `hello/`: a small Switch homebrew program built by
hand (no libnx). It checks what a game needs first and prints the results through the Switch
kernel's debug output call (`svcOutputDebugString`):

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

## Stage 4, begun: the GPU

`game/gputest.c` draws with the Switch's GPU instead of the CPU. It talks to the GPU through
libnx's nvidia services (no deko3d, no shaders): a GPU channel bound to the Maxwell 3D engine, the
window's framebuffers mapped into the GPU's address space as its render target, every rectangle a
clear limited to a scissor rectangle, and a fence that the command stream signals (a syncpoint
increment) before the frame is queued. It draws a backdrop, bouncing squares and the word GPU,
about 80 clears a frame, and sprites with the 2D engine: a 32x32 image the CPU draws, copied once
by the DMA engine, then blitted onto the frame at 1x, 2x and 3x. And a triangle drawn with shaders:
a vertex program (two attribute loads and stores: position and colour) and a fragment program (the
colour interpolated, into the render target), with their shader program headers, in Maxwell machine
code written by hand, as there's no shader compiler in this build environment. And textured shapes,
drawn as games draw sprites and backgrounds: a turning square with the sprite on it (point
sampled), and a backdrop of tiles over the bands, scrolling, blended by its alpha. The tile is
drawn by the CPU and copied by the DMA engine into a block-linear image, the GPU's own layout, as
games upload their textures; it's filtered bilinearly and repeats. And a depth test: two bars
across each other in depth, a red one drawn first and a green one, each in front on its own half.
And a square coloured from a storage buffer: its fragment program loads 16 bytes from a GPU
address it finds in the constant buffer (where NVN keeps storage buffers' addresses), which Eden's
shader recompiler turns into a storage buffer read. And lines, 3 pixels wide, and a point. And a
stencil test: a triangle drawn into the stencil only, then a square drawn where the stencil is
set, which leaves an orange triangle.

![The GPU test in shadPS4: everything on it drawn by the null renderer's software GPU](screenshots/gputest-shadps4.png)

Two ways it shows:

- **The null renderer, in software** (patch 0014; the PS4 build): colour clears are written to
  emulated memory (block linear or pitch, scissored, the common colour formats, channel masks for
  the 8-bit ones), a block of the surface at a time, and DMA copies and 2D engine blits go to their
  engines' software paths instead of being claimed as done. The sprites found a bug in Eden's
  software blitter (from yuzu): its nearest-neighbour scaling mixed the source row's fraction into
  the column, so a 2x blit came out as interleaved stripes (patch 0015 fixes it; the Vulkan
  renderer's blit was right, and the two now draw the same). So a program that draws with clears,
  copies and blits shows in the CPU-composed frames, on the PS4 now: the GPU test runs at 60 frames
  a second on the desktop and 34 in shadPS4 (its display's pace again).
- **Shader draws, in software** (patch 0016; the PS4 build): the null renderer's draws go to a
  software renderer. The vertex and fragment programs are translated by Eden's shader recompiler,
  and their IR flattened into operations on numbered cells (phis become moves on each edge of the
  structured control flow), which an interpreter runs: no JIT, so nothing needs executable memory.
  Vertices are fetched from the vertex streams (float, unorm, snorm, integer, scaled; indexed or
  not; instanced), shaded, clipped against w > 0, mapped to the window as the Vulkan renderer maps
  them, culled, and rasterized into render target 0 in emulated memory a block at a time, with the
  varyings interpolated as Vulkan interpolates them (perspective, linear, flat) and blended. The
  GPU test's first frame comes out the same as through the Vulkan renderer (below), and it draws at
  60 frames a second on the desktop. Not done yet: storage images, geometry and tessellation
  shaders, a fragment shader's own depth; a draw that needs them is skipped (or drawn without
  them), and logged once.
- **Textures, in software** (patch 0017): a texture is decoded from emulated memory the first
  time it's sampled, with Eden's own code for the GPU's layouts and compressed formats (its
  unswizzle, its BC1-7 and ASTC decoders), into 8-bit RGBA for the formats that fit and floats for
  the rest, every mip level and layer, and kept. It's checked again (its bytes hashed) after
  anything that might have written to it (a draw or clear into it, a DMA copy, a 2D blit, an
  inline upload) and once a frame, for the CPU's writes. Sampling follows the texture header and
  the sampler: nearest or bilinear filtering, mipmaps (explicit levels, and implicit ones from
  the pixel's quad: patch 0028), every wrap mode and border colours, swizzles, sRGB,
  depth compares, gathers, texel fetches, size queries; 1D, 2D, 3D, cube maps and arrays. Linear
  inline uploads, which the null renderer dropped, are written to memory now.
- **Faster, on several threads** (patch 0018): each draw's triangles are set up first (24.8 fixed
  point edge functions, so the edge two triangles share is drawn once, by the top-left rule;
  clipped to a guard band), then rasterized 8 rows at a time (a row of the render target's GOBs,
  reached directly) by a few threads: `EDEN_SW_THREADS`, by default half the processors, up to 4.
  The fragment shader runs on 16 pixels at a time, with fast paths for the commonest work: 2D 8-bit textures filtered with 8-bit weights, blending
  by source alpha into RGBA8, and 2D triangles (no perspective correction). A pixel of the GPU
  test's blended, filtered backdrop costs about 70 ns on one desktop thread, against 200 before:
  the test draws at 40-48 frames a second on one thread, 60 on four. `EDEN_SW_STATS` (always on
  in the PS4 build) logs what the draws cost every 2 seconds.
- **Depth** (patch 0019): the depth test against the depth buffer (every depth format, as the
  buffer keeps it, so the test is exact), before the fragment shader runs, so pixels behind skip
  it; depth writes; and depth and stencil clears.
- **Storage buffers and global memory** (patch 0020): shaders' storage buffer reads and writes
  (their addresses and sizes read from the constant buffers, as Eden's buffer cache reads them),
  and global memory accesses the recompiler couldn't tie to a buffer, go to emulated memory. The
  subgroup operations (votes, ballots, shuffles, the quad swizzle) work over a fragment shader's
  16 pixels (patch 0028).
- **Points and lines** (patch 0021): points as squares of the point size (fixed or the vertex
  program's), lines widened along their minor axis to the line width, as aliased lines are drawn;
  line strips and loops, quad strips and polygons too. The GPU test's lines and point come out the
  same as through the Vulkan renderer, pixel for pixel. A line loop showed that Eden's Vulkan
  renderer draws line loops as triangles (its topology table maps them to a triangle list), so
  the test draws a strip back to its start instead.
- **Stencil, and several render targets** (patch 0022): the stencil test with both faces' state
  (Eden starts with two-sided stencil on, so back faces use their own: the test's triangle found
  that), its updates on stencil fail, depth fail and pass, the write mask; a shader that can
  discard runs before the updates are made, so a discarded pixel makes none. Up to eight render
  targets, each with its own format, layout, blending and colour mask.
- **Textures the CPU writes** (patch 0023): the null renderer didn't tell its rasterizer a frame
  had been shown (the Vulkan and OpenGL renderers do), so the once-a-frame check for the CPU's
  writes never came; it does now (every 4th frame for a texture unchanged through 8 checks).
  And a write the memory manager reports marks only the textures where it landed: it reports the
  command buffers it reads, thousands of times a second, which marked every texture before. The
  GPU test's CPU paints the sprite's middle blue at frame 30, and the turning square shows it.
  The stats line counts the textures checked and decoded, and the bytes hashed.
- **GPU macros** (launcher; patches 0024, 0051): deko3d, Mesa and games draw through the 3D
  engine's macros (MME), on the GPU thread. Eden's macro JIT made its code memory executable with
  mprotect, which a PS4 app can't do, so until 03.50 the launcher had Eden interpret macros. Patch
  0051 maps each macro's code read/write/execute once on the PS4 (as dynarmic's code is), and the
  JIT is used everywhere (`EDEN_PS4_MACRO_JIT=0` interprets). Each compiled macro also carried two
  arrays of 65536 labels, 2 MiB: they are sized to the macro now, and freed once it is compiled.
  The GPU test's bouncing squares are cleared by a macro of its own (it sets the clear colour
  and clears, from its parameters). And the null rasterizer reports the writes the memory manager
  gathers (constant buffer updates among them) before each draw and clear, as the Vulkan one does:
  they gathered without end.
- **Indirect draws, inline indices, DrawTexture** (patch 0025): draws whose parameters are in a
  buffer (each read, with their count, and drawn), indices sent in the command stream (the storage
  buffer's square is drawn with them; they were read from memory before), and the 3D engine's
  textured rectangle (NVN's DrawTexture).
- **Less work per draw** (patch 0026): finding a draw's shaders meant Eden's shader environment
  hashing their code, about 60 microseconds a draw (a game drawing a sprite a draw, a thousand a
  frame, would have spent 60 ms on it); a stage's program is now remembered by its address and
  hashed again once a frame, or when something writes over its code. Clears mark the bytes they
  write, not a generous range.
- **Small draws on every thread** (patch 0027): a sprite is a draw too small to share between
  threads, so 2D games' draws ran on one. Draws are now set up and queued, and the queue is
  rasterized together, a GOB row at a time across the threads, each row in draw order (the pixels
  come out as if drawn one by one). It's rasterized before anything that could see or change what
  it draws: a clear, a DMA copy, a 2D blit, an upload, a fence or semaphore, the frame being shown,
  a texture it draws to being read, a shader that writes memory. The GPU test's stress test (A:
  400 more sprites, each a draw; up and down change how many) went from 29 frames a second to 55
  on four desktop threads.
- **Quads, mipmaps, branches** (patch 0028): a fragment shader's 16 pixels are now 8 columns by 2
  rows, four 2x2 quads as the GPU shades them, with helper pixels where a quad sticks out of the
  triangle. The Switch's GPU has no derivative instruction: NVN's code works derivatives out with
  shuffles between a quad's pixels and its swizzled add (FSWZADD), and the texture unit picks an
  implicit level of detail from the quad. Both see the quads now, so mipmaps work. A program that
  branches ran a pixel at a time; now its pixels each go their own way, the ones at the earliest
  instruction running it while the rest wait, rejoining where they meet (results kept only for the
  pixels that ran an instruction), and local memory is per pixel. The texture fast paths now take
  repeating (power-of-two) and clamped coordinates, which sent every lane of the tiled backdrop
  down the slow path before, and mipmapped textures a level at a time with trilinear blending: a
  pixel of the GPU test costs 25-28 ns on one desktop thread, from 38. The test has a floor in
  perspective now (bottom middle): its texture's levels are a colour each, so where each is
  sampled shows, and its shader branches per pixel for its stripes. It matches the Vulkan renderer
  on lavapipe to within a few levels of each colour channel.
- **Setting draws up** (patch 0029): a program remembered at an address was checked again each
  frame through Eden's shader environment, whose reads of the code (the memory manager's safe
  reads) each ask the rasterizer to flush that region, which rasterized the queued draws halfway
  through a draw's setup. The program's bytes are kept and compared instead, and a read of memory
  rasterizes the queue only if a queued draw draws there. A draw's setup went from 157 to 31
  microseconds on the desktop; the GPU test draws at 53 frames a second on one desktop thread.
- **Writes and filtering in arrays, a shader's own depth** (patch 0030): a profile showed the
  rasterizer spending most of its time writing pixels one at a time. In a block-linear RGBA8
  render target, a block's 16 pixels are two runs of 32 bytes (4 columns by 2 rows, as a GOB keeps
  them), so they're converted, blended and stored in loops the compiler vectorizes now. Bilinear
  filtering does two channels in each 32-bit multiply; sRGB textures, which went a pixel at a time,
  take the fast path (decoded through their table, filtered in floats). And a fragment shader that
  writes its own depth gets the depth test after it, with that depth. A pixel of the GPU test costs
  about 22 ns on one desktop thread (60 frames a second; the stress test 37). The test has two
  squares whose fragment programs write opposite depths, and a square of the tile through an sRGB
  texture header (top right); both match the Vulkan renderer.
- **Compute shaders** (patch 0031): the compute engine's dispatches run, which nothing did before:
  the program translated as a draw's stages are (through Eden's compute environment), constant
  buffers from the launch description, textures from the compute engine's pools, storage buffers.
  A workgroup's invocations run 16 at a time, with shared memory, their ids, and atomics on shared
  memory, storage buffers and global memory. A program with barriers runs each group of 16 to the
  barrier, keeping where it stopped, then the next group, until all are past it. The GPU test
  dispatches a compute program of its own every frame (4x4 workgroups of 8x8: each invocation puts
  a colour in shared memory and, after a barrier, writes its mirror image's to a texture), shown on
  a square at the top. Eden's Vulkan renderer shows that square black: its texture cache doesn't
  see a storage buffer's writes to a texture's memory, so the test checks every texel against what
  they should be instead.
- **More of the 3D engine's fixed functions** (patch 0032): primitive restart (an indexed draw's
  runs between restart indices), the provoking vertex register (first or last, for flat
  varyings), polygon offset (the depth bias: a constant and a slope factor, clamped, for the kinds
  of primitive it's enabled for), the alpha test, and point coordinates (gl_PointCoord) across
  points. The GPU test shows three of them on the right: the tile with the alpha test, a strip of
  two quads with a restart between them, and a point coloured by its coordinates; they match the
  Vulkan renderer. (A restart index other than all ones is drawn as the GPU would, but Eden's
  Vulkan renderer can't: Vulkan restarts only at all ones.)
- **Multisampled render targets** (patch 0033): a multisampled target's width and height count
  samples, its pixels a grid of them (2x2 for 4x), and software draws went into the grid's top-left
  corner at the pixels' resolution. They're drawn at the samples' resolution now (the viewport,
  scissor, points and lines scaled, each sample shaded, gl_FragCoord still in pixels), so a resolve
  averages real samples.
- **A 3D workload, and what it showed** (patches 0035-0038): the GPU test's B turns on a second
  stress test, as a 3D game draws: 12 large quads over each other at their own depths, depth-tested,
  each textured in perspective and lit per pixel (a normal normalized, a light: 33 interpreted
  ops). Profiled, it pointed at four things, each fixed: the depth test, a pixel at a time (now 16:
  a block's depths read, converted, compared and written back as two runs of 32 bytes, for the
  commonest case, no stencil and a 4-byte block-linear buffer); varyings interpolated for every
  component though shaders read fewer, and in a longer form than needed (now only those read, in
  plane form); and the interpreter spending a third of its time copying Eden's Identity
  instructions' values, 256 bytes each (now they share their argument's cell, and the moves left
  copy only what a value takes). The 3D test went from 26 to 37 frames a second on one desktop
  thread (12 ns a pixel of its triangles' bounding boxes, from 18), and the 2D scenes gained about
  10%. `EDEN_SW_DUMP=1` logs each program's ops.
- **Storage images and texel buffers** (patch 0039): shaders' image loads, stores and atomics read
  and write the texel in emulated memory where its texture header says it is (pitch or block
  linear, the view's level and layer), converting from and to the image's format (formatted) or
  moving its bytes as they are (typed); texel buffers are fetched the same way. Textures a shader
  may have written are checked again afterwards. The GPU test's second compute program stores
  diagonal bands of colours from a texel buffer into a block-linear storage image, shown below the
  first compute square; Eden's Vulkan renderer on lavapipe draws it the same, texel for texel.
- **Software shaders compiled to native code** (patch 0040): a fragment or compute program's runs
  of instructions between branches are compiled to x86 when it's translated (Xbyak, AVX at 128
  bits, which the PS4's Jaguar cores have), into memory mapped readable, writable and executable,
  as dynarmic's code is on the PS4. A run's arithmetic is done four lanes at a time, a value to a
  register; textures, memory and instructions that mix lanes call the interpreter's code for all
  16. Where lanes have branched apart, the interpreter steps through the run instead. The 3D test
  went from 37 to 43 frames a second on one desktop thread (9.6 ns a pixel, from 11.6), the 2D
  scenes 10% faster; the pictures are the same, byte for byte. `EDEN_SW_JIT=0` interprets.
  `EDEN_SW_JIT_TEST=1` runs every instruction it compiles on awkward values (NaNs, infinities,
  zeros of both signs, denormals, edge integers) against the interpreter (test.sh does).
- **Multisampled texel fetches, and varyings only where they're needed** (patch 0041): a shader's
  texel fetch from a multisampled texture reads its sample from the pixel's grid of samples (the
  GPU test shows the storage image again as a 16x16 texture of 2x2 samples, each pixel's bottom
  right: as Eden's Vulkan renderer draws it); and varyings are interpolated after the early depth
  test, not for blocks it rejects (4% on the 3D test).
- **Vertices shaded once, 16 at a time** (patch 0042): a draw's distinct vertices are shaded once
  each (an indexed mesh uses each about six times), 16 at a time so vertex programs run in native
  code too, and each one's window coordinates and clip planes are worked out once for all its
  triangles. The GPU test's X turns on a vertex stress test (16 indexed meshes of 4225 vertices,
  each transformed by a matrix in a constant buffer): 5 to 17 frames a second on the desktop.

![The GPU test's first frame: Eden's Vulkan renderer on lavapipe (left), the software renderer (right)](screenshots/gputest-vulkan-vs-software.png)
- **Eden's Vulkan renderer, headless** (patch 0013; the desktop, `EDEN_PS4_RENDERER=vulkan`): the
  renderer makes a surface that shows nothing (`VK_EXT_headless_surface`) and reads each frame back
  for the launcher instead of presenting it. With lavapipe, Mesa's Vulkan driver that runs on the
  CPU, the GPU test draws at 58 frames a second, through Eden's whole GPU path: the Maxwell engine,
  its texture cache, its Vulkan renderer and presenter. That is the CPU-Vulkan route below, shown
  working; what's left for the PS4 is a CPU Vulkan built for it.

![The GPU test through Eden's Vulkan renderer on lavapipe](screenshots/gputest-vulkan-lavapipe.png)

## Stage 5: the PS4's own GPU (in progress)

Crema, the Wii U emulator this repository builds, already runs Vulkan on the PS4's GPU: Mesa's RADV
with a GNM winsys (`../port/mesa/ac_ps4_drm.c`, `../patches/mesa-ps4*.patch`,
`../scripts/build-mesa.sh`). Eden4 (github.com/xatyy/eden4, GPL-3.0-or-later) showed Eden's Vulkan
renderer working on that kind of stack on a console, on this same Eden commit. Patch 0043 brings
its Eden side here: RADV linked into the executable, a VideoOut swapchain whose flips are queued
behind the GPU's work, and GPU memory budgeted from the direct memory left after the Switch's
4 GiB. The launcher uses it whenever RADV is linked (`src/CMakeLists.txt` links it when
`~/cemups4-build/build/mesa` has been built): it gives the screen (video out and its direct
memory) to the program's display while the program runs, and takes it back after.
`EDEN_PS4_RENDERER=null` in `settings.txt` keeps the software renderer. Without RADV in the
build, nothing changes.

Commercial games: the start page lists every `.nsp`, `.xci`, `.nso` and `.nro` in
`/data/eden-ps4` and at the top of a USB drive (`/mnt/usb0` to `/mnt/usb7`), with each file's
size and where it is (a long list scrolls). Eden needs the keys of the Switch it was dumped from
in `/data/eden-ps4/user/keys/prod.keys` (and `title.keys`) and that Switch's firmware (the NCA
files) in `/data/eden-ps4/user/nand/system/Contents/registered/`; without `prod.keys` the start
page says so when a game is chosen, instead of booting it.

Getting them there (03.40): **Games and Keys, Import from USB** (Square). Without root access on
the console (PS4-Xplorer on 13.52 with GoldHEN says "Not Supported"), file managers can't write
the app's data folder, but the app itself reads USB drives (`src/usb_import.cpp`): `prod.keys`
and `title.keys` from the top of a drive, a folder at its top (Lockpick_RCM writes them to
`switch/`) or that folder's `keys` folder; the firmware's `.nca` files from the folder (at the
top, or one below) that has the most of them, e.g. `Firmware 19.0.1`. The firmware already there
is replaced, as Eden's own firmware install does; files are copied through a `.part` name, and a
message shows the progress and what was copied from where. GoldHEN's FTP server (port 2121, from
its settings) also reaches `/data/eden-ps4`.

One program after another (03.40): the first console run (13.52, GoldHEN) crashed when a second
program started in the same session, without a crash log, its C heap grown by six 32 MiB
overflow heaps. Three things, found on the desktop with `EDEN_PS4_BOOTS=<n>` (several boots in
one process, the heap in use after each):
- Eden kept the host fibers (two stacks each) of the ~66 kernel threads still referenced at
  shutdown: 264 MiB per program on the desktop, 66 MiB on the PS4 (patch 0048: the kernel's
  shutdown releases them for every live thread). The heap now stays flat from one program to the
  next (desktop: 3 MiB after each of three boots, software or Vulkan).
- Stopping a program on Vulkan used freed memory: `~RasterizerVulkan` submitted the last recorded
  work after `frame_gen` and `applet_frame` were destroyed; lavapipe crashed every time (patch
  0047: `~RendererVulkan` finishes the scheduler first).
- RADV's PS4 winsys made a new GPU device for every Vulkan instance (one per program), each with
  new GPU address windows (16 + 8 GiB), and kept the last one's buffers in quarantine (up to
  96 MiB of direct memory) forever. One device now lives for the whole process; when its last
  user goes, the GPU has finished, the quarantine is released, and the boot trace says how much
  is still allocated.

The boot trace now has a `memory before booting` and `memory after the program` line (C heap in
use, flexible memory free, the largest free block of direct memory), and the last session's
`gpu/radv-ps4.log` is kept as `radv-ps4-previous.log` (a crash restarts the app, which started a
new log over it).

04.06: **less sleeping on Eden's host locks.** 04.05 on a console (the tester's slowest track,
50%, Profiler on): 30-second windows 29.5-31.5 fps (mean 30.5), 10-second ones 28.2-32.0, a dip to
24.8 at a race start. The helpers' work a frame fell from 20.7 to 18.4 ms, so the game's main thread
alone is the limit now: busy 78-80%, waiting 6.2 ms a frame, its own work about 25.8 ms a frame. Of
emulated core 0's host time in a race, 62% is translated code, 29% Eden (a fifth of it the idle
spin) and 9% the PS4's kernel, nearly all of it (8.3%) in `_umtx_op`: host threads asleep on a lock
or condition variable while the core had work. The main thread makes about 1,100-1,370
`ArbitrateLock`, 2,100-2,340 `ArbitrateUnlock` and 780 `WaitProcessWideKeyAtomic` calls a second
(about 11 us an unlock), and each takes Eden's scheduler lock, the hardware timer's, and wakes or
interrupts other cores (their context locks, CoreTiming's event queue). All of those are a
`std::mutex`, which on the PS4 sleeps in the kernel as soon as it is found taken; the wake costs
tens of microseconds, the locks are held for a few.

- Patch 0093, `common/ps4_spin_lock.h`: `Common::SpinThenLock` (and `SpinLockGuard`): on the PS4, a
  lock found taken is tried again (8 `PAUSE`s between tries) for up to 20 us, then slept on.
  Bounded, unlike 04.00's: a waiter never keeps a holder off its core for longer than that. Used by
  `KSpinLock` (the scheduler lock, the hardware timer's, the other kernel spin locks),
  `PhysicalCore`'s context lock (`m_guard`: entering and leaving a thread, `Interrupt`,
  `ClearInterrupt`, the idle sleep), and CoreTiming's event queue lock. Other platforms lock as
  before.
- `CoreTiming::ScheduleEvent` wakes the timing thread only when the new event is now the earliest
  in the queue: the thread waits for the earliest one and sees later ones when it wakes (the
  hardware timer's reprogramming, every guest sleep and timed wait, schedules one).
- The 30-second log line counts, for each lock class, how often it was found taken a second and
  how it was got (spinning, us each / slept on, us each), and the timing thread's wakes a second
  for new events and those skipped (later events).

04.05: **constant offsets in fastmem accesses, float compare branches, no redundant
extensions.** 04.04 on a console (50%, Profiler on): the tester's slowest track at 28.2-30.9 fps
(median 29.6), another track 29.7-32.9 (30.8), 04.01's usual track 26.7-28.9 (28.1): the main
thread's work a race frame 27.1 ms on 04.01, 25.3 ms on 04.04's slowest track; flag code gone from
93% of the hottest blocks' samples. The tester's target: 30 minimum, 32 average. What the
translated code of compiled ARM C executes, instruction by instruction (callgrind with
`--dump-instr`, the harness's `DUMP_JIT` and perf map; `cgjit.py`-style classification): guest
register loads and stores 23-29%, LEAs 11-16% (2-7% an address used by the next access alone),
a float compare and its branch ~10% of a float-heavy program (`ucomiss`, then `sete/rcl/shr/pext`
into the FPSCR's and CPSR's NZCV, `cmp al; sahf; jcc`), a `movzx` after every LDRB/LDRH and before
every STRB/STRH. In Mario Kart's 60 hottest blocks a third of the guest instructions are loads and
stores at a register plus a positive constant (and none is a float compare).

- Patch 0092, `UserConfig::fold_address_offsets` (on in Eden): before a block is emitted
  (`A32EmitX64::FoldAddressOffsets`), a plain fastmem load or store through `Add32(base, 1..65535)`
  used by it alone takes the base and puts the constant in its displacement: `[r13 + base + 8]`,
  no LEA. A fault there fake-calls a per-access stub (`push base; lea base32, [base + 8]; call` the
  fallback; `pop; ret`): the address whole with the 32-bit wrap (an address past 4 GiB faults in
  the arena, which reaches 2^39; negative constants aren't folded, they could go below it).
- `UserConfig::skip_redundant_extensions` (on): a zero extension of a byte or halfword just loaded
  is the load's value; a byte or halfword store of a word's low bits stores from the word's
  register.
- Float compares: `CondFlagsFP32/64`, `ucomis` of the operands in the order the condition needs
  (swapped for MI, PL, CS, CC, HI, LS), one jump (EQ and NE also test the parity: unordered). The
  look-ahead also tells whether the FPSCR's NZCV is dead at a block's exit (a VMRS reads it; VCMP,
  VMSR set it; calls and returns as for the flags): its stores go, and the compare with them.
  `vcmp; vmrs APSR_nzcv, fpscr; bgt` is `ucomiss; ja`. With the call standard the FPSCR's NZCV
  isn't preserved across a call or a return (as the standard has it): a function that reads the
  whole FPSCR there (`fegetenv`, a rounding mode change) may see stale condition bits, which it
  carries along unread. The random programs that read it there (`GEN_ABI=1 gen10.py`, 3 in 400)
  differed until they masked those bits, as such code does; exact mode is exact.
- Host harness (valgrind's instruction count, 04.04 -> 04.05): game-like C 122,570 -> 107,047 a
  frame (133,744 on 04.01), zlib 21.94M -> 20.04M (27.58M), Mario Kart's hottest loops 537 -> 469
  an iteration (760). Old vs new: more than 5,000 random A32 programs equal to 04.01 (gen, gen2-4,
  gen7, gen9, gen10 with more float compares and FPSCR reads; fastmem, split stack; calls with and
  without the call standard), `programs/wrap.s` (addresses past 4 GiB, the marked and unmapped
  pages), zlib and the C benchmark at -O1, -O2, -O3 and -Os. Translation at scale as 04.04.

04.04: **calls and returns end the flags; a live block's branch tests its compare.** 04.03 on a
console (Mario Kart, **100%** resolution, Profiler on): races at 26-30 fps, 28-29 most of the
time, as 04.01 at 50% (the PS4's GPU 14% busy, 10% at 50%; Eden's GPU thread waiting for it 10-12%
of the time, 6-9% at 50%). The main thread ran 73-74% (76%), waited 26% on its helpers. In
`hotblocks.log`, the dead flags had gone from the byte copy and the test chain (the loops
`programs/flagloops.s` copied), but two thirds of the hottest blocks' samples were still in blocks
storing and reloading them: the fill and copy loops (memset, memcpy), an indexed copy,
compare-and-branch blocks. Their successors lead to a return (memset's tail sets N, Z and C, then
`bx lr`: V never set) or a call through a register within a few instructions, and the look-ahead
counted those as reads.

- Patch 0091: `UserConfig::elide_dead_nzcv_abi` (on in Eden): a call (BL, BLX) or a return (BX LR,
  POP/LDM SP with the PC, LDR PC, [SP], #4, MOV PC, LR) ends a look-ahead path with the flags set:
  the procedure call standard has them undefined on entry to a function and on return from it.
  Compiled code keeps to that; the run-time ABI's helpers that return flags (`__aeabi_cdcmple`...)
  set them in the block that returns (not one that links). Other jumps through registers (switch
  tables) and SVC still count as reads. And a block ending in a conditional branch whose flags are
  live keeps its store but branches on the compare (`cmp; jcc`) instead of loading the flags back.
- Host harness, valgrind's instruction count a frame (04.01, exact, with the call standard):
  game-like C 133,744, 124,601, 122,570; zlib (deflate and inflate, `programs/c/zbench.c`) 27.58M,
  22.03M, 21.94M. Exact mode: 1,600 random A32 programs equal to 04.01. With the call standard:
  600 random programs that keep to it (`GEN_ABI=1 gen10.py`: callees set the flags before reading
  them, callers after a call, every kind of return) and zlib and the C benchmark built at -O1,
  -O2, -O3, -Os and -O2 -fno-inline equal to 04.01; programs that break it (a function reading its
  caller's flags) differ, 4 in 100, as they should. Translation at scale (`manyblocks.py`) as 04.03.

04.03: **04.02's look-ahead registered as runs.** 04.02 on a console (Mario Kart, 50%, Profiler
on): no frame in the minute before it was stopped (L3 + R3). The recompiler translated 1,196
blocks a second in 536 ms in the first 30 s, then 651 in 871 ms (04.01: 3,061 in 346 ms, 113 us a
block): 1.3 ms a block and rising. Core 0 ran the game's code 9% of the time and Eden's 75%, most
of it `boost::icl` (`block_ranges`, an interval map of sets of blocks). 0089 registered the code a
block's look-ahead read as one range from the lowest address to the highest: after a call, a
megabyte or more, so every block's range overlapped thousands of others and each insertion grew
thousands of sets.

- Patch 0090: each run of consecutive instructions the look-ahead read is a range of its own
  (`IR::Block::lookahead`, at most 16 runs; past that the flags count as live). The code it
  produces is 0089's.
- Host harness: `manyblocks.py`, 3,000 functions calling leaves a megabyte away, run once (its
  time is the recompiler's): 0.40 s with 04.01, 18.3 s with 0089, 0.42 s now; 12,000 functions
  1.38 s with 04.01, 1.52 s now. Old vs new: 1,500 random A32 programs equal. `IR::Block` keeps
  29 instructions inline instead of 30 (the PS4 lays it out larger than the desktop: the run list
  didn't fit its 4096 bytes). The harness's tests were small programs: translation cost at a
  game's scale needs this one.

04.02: **dead flags at a block's exit are not stored.** 04.01 on a console (Mario Kart, 50%,
Profiler on): races at 26-30 fps (27-30 with the Profiler off, the run before), within a frame of
04.00: the 11-19% fewer instructions didn't show. The main thread runs 76% of the time and waits
23% on its two helpers (61% and 44% busy); core 0 is 58% translated code. The profile's offsets
inside blocks: a third of the samples that land in translated code land in a block's first 16
bytes. The 60 hottest blocks run 5.85 host instructions per guest instruction: 27% loads and
stores of the guest registers in `JitState`, 9% flag handling. Almost every block ending in a
conditional branch stored the flags after its compare (`xor; cmp; cmc; lahf; seto; mov [r15+76]`)
and loaded them back for the branch (`mov; sahf; cmc; ja`); a TST cost nine instructions to
merge N and Z in. The code a branch leads to almost always sets the flags before it reads them.

- Patch 0089: a block ending in links (B<cond>'s If terminal, a branch, a call, the next
  instruction) reads the ARM code its successors start with (`FlagsLookahead` in
  `translate_arm.cpp`): along every path, following branches and BL into the called code, a flag
  read before it is set again makes the flags live; a return, a jump or call through a register,
  SVC, MRS/MSR or anything else it doesn't know counts as a read; a branch target reached again
  with no more flags unset adds no path; 64 instructions and 6 branches at most. When the flags are
  dead, `A32DeadNZCVPass` drops the stores after the block's last flag read, and a conditional
  branch at the end tests the compare's values (`CondFlagsSub32/Add32/Test32/NZ32`: `cmp a, b`,
  `jcc` on the host's flags). The code read joins the block's ranges for invalidation.
  `UserConfig::elide_dead_nzcv`, on in Eden; off with cycle counting and halts on memory
  accesses. A first version took a call to clobber the flags (the procedure call standard): a
  random program whose function read its caller's flags caught it; calls are followed now.
- Host harness (Eden's options, fastmem; valgrind's instruction count): Mario Kart's hottest loops
  (`programs/flagloops.s`: the fill, copy, byte copy, test chain and indexed copy from 04.01's
  `hotblocks.log`) 760 to 536 an iteration; game-like C compiled for ARM with clang -O2
  (`programs/c/game.c`: entities, a sort, hashing, a tree) 135,555 to 126,310 a frame; calls,
  PUSH/POP and struct accesses unchanged (their loops end next to an SVC). Old vs new: 3,200 random
  A32 programs equal (gen2, 3, 4, 7, 9 and the new gen10, flags across blocks; ARM, Thumb, fastmem,
  split stack).
- `tools/dynarmic-harness`: `gen10.py` and `dtxa.sh` (ARM-only differential tests), `DYN_NZCV`,
  `test` mode dumps code too (`DUMP_JIT`); `wrap/` takes xbyak and fmt from `~/eden-deps` when no
  installed package has them.

04.01: **LEA into a new register; flags from the register.** 04.00 on a console (Mario Kart, 50%,
Profiler on): races at 24-29 fps, no freeze in ~6 minutes. The main thread 76% running, 24%
waiting on its helpers; the game cores 48-58% translated code, Eden's own code under 2% besides
idling; writes to GPU-cached memory through the callbacks (up to 46,000 a second) don't show in the
profile (cheap); Eden's GPU thread ~65% busy. The hottest loop (an unrolled memset) showed each
`str r1, [r3, #N]` as three instructions: the base copied out of the way
(`RegAlloc::UseScratchGpr` of a value still live), `lea` on the copy, the store.

- Patch 0088: `EmitAdd` (no flags) with an operand used again: `lea result, [op1 + imm]` or
  `[op1 + op2]` into a new register (`RegAlloc::IsLastUse`); `LogicalShiftLeft32` by 1-3 of an
  operand used again: `lea result, [op * 2/4/8]`; `EmitCond` right after `A32SetCpsrNZCV` (nothing
  emitted between) uses the register just stored instead of loading `cpsr_nzcv` back.
- Host harness (Eden's options, Jaguar features, fastmem; callgrind instructions an iteration):
  indexed loop 14,387 to 12,852, unrolled memset 10,579 to 9,053, calls with PUSH/POP 561 to 465,
  PUSH/POP 505 to 407, struct accesses 91 to 77. Old vs new: 2,000 random A32 programs (gen2, 3,
  4, 7, 9; ARM, Thumb; fastmem, split stack) and 350 A64 (gen6, 8) equal; `test.sh` 0 failed.
- `tools/dynarmic-harness`: `link.sh` compiles in the options Eden uses (`HAVE_*`; without them the
  `DYN_*` switches did nothing), `bench` mode dumps code with `DUMP_JIT`.

04.00: **statistics locks that sleep: a 20-second freeze.** 03.99 on a console (Mario Kart, 50%,
Profiler on): races at 25-29 fps (23-26 on 03.98); fastmem's "pages not back to back" declines 78
a second at boot then 0-3 (from 2,000, 1,200, 400-600, 150). The main thread still ran 75% of a
race and waited on its helpers 24%. Five minutes in, no frame for 20 s: cores 0-2 idle, core 3
busy 74%; hang.log had `nvservices` at `NVDRV::Ioctl1+0x184` and `audio` in
`HandleSyncRequest`'s scope guard, both in the `xchg`/`test`/`jne` loop on
`PS4Perf::service_lock`. The services' host threads (priority 640, module 1) spun while the
lock's holder, of a lower priority, got no core; the game waited on the GPU driver until the hang
watchdog's signals (thread stacks) let the holder run.

- Patch 0087: `PS4Perf`'s service-call table, idle accounting and guest-thread table use
  `std::mutex` (a waiter sleeps, the holder runs). `Orbis::ArenaPaddingCount` reads the count
  without `arena_lock`. `core_switch_ns` a cache line a core.
- `port/ps4_runtime.cpp`: `__wrap_clock_gettime` no longer increments `ps4_clock_reads` (no
  reader; the idle cores' spins read the clock every microsecond or so, moving its cache line
  between cores).
- Seen, not changed yet: writes to memory the GPU caches through the callbacks climb through a
  race (1,200 to 65,000 a second), mostly core 1's `memcpy` into GPU buffers.

03.99: **the game's code region lined up with the PS4's pages.** 03.98 on a console (Mario Kart,
50%, Profiler on): races at 23-26 fps. The busiest threads in a race: the main thread (core 0,
priority 44) running 76% of the time, waiting on a condition variable 23% and on locks 4%; two
helpers at priority 44 (core 2: running 61%, waiting 38%; core 1: 43%, 48%), a job system; a
priority-32 thread on core 1 running 12%, waiting on objects 87%. The profile's race windows on
core 0: about 61% translated code with no hot spot (the busiest block 0.2%), 8% Eden's own code
(the scheduler, SVC arguments), the rest idle (spinning in `PhysicalCore::Idle`, its
`clock_gettime`, or asleep). In the hot blocks of today's build, a getter's literal-pool load,
GOT load and global's load walked the page table while its heap loads used fastmem; fastmem had
declined "pages not back to back" about 135,000 times in the session (2,000 a second at boot,
400-600 in menus, 150 at a race's start). The loaders put the code region at 0x200000 plus a
random 4 KiB step (`aslr_offset`, "this is bad form of ASLR"), while its memory comes in blocks
at their own size's boundary: three times in four every 16 KiB of the game's code, constants and
globals was out of line with the DRAM by 4, 8 or 12 KiB.

- Patch 0086: on the PS4 the loaders' (`deconstructed_rom_directory.cpp`, `nro.cpp`) ASLR offset
  is a 16 KiB step.
- Patch 0086: `KMemoryManager::AllocatePageGroupImpl` gives the rest under 64 KiB as one run at a
  16 KiB boundary (`AllocateAligned`) instead of single 4 KiB pages anywhere (a code region's
  end, often its globals); the PS4 and `EDEN_TEST_ARENA_LINE_UP` desktop builds. Desktop:
  `test.sh` 0 failed, the run taken (logged in test builds).

03.98: **which threads a frame waits on.** 03.97 on a console (Mario Kart, 50%, Profiler off): races
at 24-28 fps, as 03.96 (a different track); core 0 idle 29%, cores 1 and 2 36% and 30%. Core 0's
threads' system calls a second: `WaitProcessWideKeyAtomic` 281 ms (794 calls: the main thread
waiting on a condition variable for other threads' work), `ArbitrateLock` 39 ms (1,091 calls,
35 us each), `SendSyncRequest` 25 ms (80 calls, 306 us each), `ArbitrateUnlock` 23 ms (1,924
calls); core 2's `ArbitrateLock` 1,169 a second too (a lock taken in turns with core 0). Writes to
memory the GPU caches through the callbacks climbed from 1,000 to 51,000 a second in a race. Core
0's code cache (48 MiB) filled after 6 minutes and was flushed during the next loading screen.
The 30-second line was cut at 2 KiB by the boot trace (the busiest services were lost).

- Patch 0085: `PS4Perf::guest_threads`, a table by thread id: `KScheduler::SwitchThread` adds the
  thread switched out its time on the core since the core's last switch; `Svc::Call` adds each
  call's whole time to its thread's waits, by kind (condition variable, lock, synchronization,
  sleep, service call, address). The 30-second line lists the eight busiest threads (core,
  priority, % running, % in each kind of wait); slots of threads that did nothing in the period
  are freed.
- The launcher writes the 30-second line in pieces of up to 1,800 characters, cut at "; ", each
  after the first marked "(continued)".

03.97: **VLD1/VST1 a whole register at a time; longer idle spins on the game's cores; what the
cores wait on.** 03.96 on a console (Mario Kart, 50%, Profiler off): races at 25-29 fps (34-40 ms
of work a frame), from 20-23; fastmem's "pages not back to back" declines 0-1 a second (73 of 76
aliases put on copies); core 0 idle 19%, cores 1 and 2 39% and 52%; the PS4's GPU busy 10%. The
old profile's race windows: core 0's time spread over hundreds of blocks (no hot spot), its Eden
code mostly the idle spin. In the hot blocks, VLD1/VST1 were translated element by element
(`vld1.8` of a Q register: sixteen byte loads, shifted and ORed into the register file) and a
`vld1.64 {d6, d7}` used by the next instruction as Q3 went through memory (two 64-bit stores, a
128-bit load: a store-forwarding stall on Jaguar).

- Patch 0084, dynarmic: `v8_VLD_multiple` / `v8_VST_multiple` with one element a structure
  (VLD1/VST1 of 1-4 registers), little-endian: one 64-bit access a D register at base + 8r; an
  even D register and the next as one Q value (`Pack2x64To1x128` into `SetVector`, `GetVector`
  and `VectorGetElement64` out), so get/set elimination keeps it in a host register. Host
  harness, a 4 KiB `vld1.8`/`vst1.8` copy plus 256 `vld1.32`/`vmla.f32`/`vst1.32` steps: 7.7 us
  before, 2.1 now (1.3 with fastmem). `gen9.py` (random VLD1/VST1 of every size, register count,
  start, alignment hint and writeback, at unaligned and page-crossing addresses, with VLD1 lanes,
  VLD2-4, VLDM/VSTM, VLDR/VSTR and NEON/VFP arithmetic): old build vs new, 600 programs (ARM and
  Thumb) and 400 with fastmem equal, single-stepped too; `gen7.py` 504 equal.
- Patch 0084: `PhysicalCore::Idle` spins 1 ms on emulated cores 0-2 when threads are pinned (each
  has a PS4 core to itself), 50 us on core 3 and when not pinned.
- Patch 0084, diagnostics: `PS4Perf::svc_cores` counts each SVC's calls and whole time by the core
  it was made on (`Svc::Call`); the 30-second line gives each of cores 0-2's four longest. nvdrv's
  `Ioctl1` adds each ioctl (`nvdrv ioctl`, its command in hex) to the busiest calls.

03.96: **delete games on the PS4; copy keys and firmware to it.**

- OPTIONS on a title on the PS4's own storage (under `/data`: the data folder, or a Game Folder
  there) offers Delete between Close and Start (a game on a drive still offers Copy to the PS4).
  Delete asks first ("Delete <name>?", Cancel selected), the dialog holding the file's path (the
  titles may be read again meanwhile); then the launcher removes the file (only a .nsp, .xci, .nro
  or .nso under `/data`) and its cached name and icon, and says how much was freed. Saves stay.
- Games and Keys: Square is "Copy keys and firmware to the PS4" (the import from USB drives and
  `/data/eden-ps4/import` that was there, now said for what it does). Its rows say when the keys
  or firmware are used from a drive whether the PS4 has its own copy, else that Square makes one;
  the report says games start without the drive afterwards. With no drive plugged in, a boot
  uses the PS4's copies (`SetAppDirectory` resets the keys folder at each boot).
- `src/ui/preview.cpp`: the info dialog with Delete and its confirmation.

03.95: **thread stacks lined up with the PS4's pages, so fastmem reaches them; more code cache for
core 0.** 03.94 on a console (Mario Kart, 50%, Profiler off): races at 20-23 fps against 03.93's
22-24 (75%, Profiler on), core 0 busy 88% (40.9 ms a frame against 36.6), cores 1 and 2 5-7% more
a frame; fastmem declines in races down to 5-30 a second from 60-90, translation 15-60 blocks a
second; core 0's code cache 5 MiB free of 40 after 7 minutes (two whole flushes while loading),
core 1's 6.6, core 2's 20, core 3's 8 never used. Walking every stack access from the start cost
more than the declines it saved.

- Patch 0083, kernel (`k_page_table_base.cpp`, the PS4 and `EDEN_TEST_ARENA_LINE_UP` desktop
  builds): `MapMemory` of heap memory (`svcMapMemory`: nn::os and libnx thread stacks) up to 8 MiB
  maps the alias on a copy of the source (`CopyAliasForArena`): memory from
  `AllocateAndOpenContinuous` in whole 16 KiB, the alias at its own place in a 16 KiB page, the
  rest of its first and last 16 KiB spare pages, zeroed. The copies come from the applet pool
  (HLE applets leave it unused), at most 128 MiB: the application's pool is all the game's (its
  `PhysicalMemoryMax` is the pool's size), and a game that takes all of it must still get it. The source keeps its pages (locked, as on
  the Switch); `UnmapMemory` of the alias, or any part of it, copies its data back to them
  (`CopyBackAlias`, in place of `IsValidPageGroup`), so the heap stays lined up for fastmem too.
  Skipped when the source already lines up with nothing free beside the alias. A stack the
  kernel allocates (`MapPages`, the main thread's) of whole 16 KiB goes at a 16 KiB boundary, on
  one piece of memory (`AllocateAndMapPagesImpl`: `AllocateAndOpenContinuous` for 16 KiB-aligned
  mappings of whole 16 KiB up to 8 MiB).
- Patch 0083, the arena (`orbis_memory.cpp`): padding, a table of 16 KiB pages whose free guest
  pages (bit mask) are the copy's spare pages: `ArenaPageSource` takes an unmapped guest page
  there as that spare page, so the alias's first and last 16 KiB (a stack's top, its busiest part)
  map too. The table is fixed (1,024 entries, read in the fault handler under `arena_lock`); the
  kernel adds an alias's padding once it's mapped and takes it out (then frees the pages) when
  the alias goes, or all of it when the process ends. A guest access past a stack's end, where the
  Switch would fault, reaches the spare page instead.
- Patch 0083, dynarmic: a declined stack access only counts; walking every stack access from the
  start (03.94) begins after 32 on a core (`StackDeclinesBeforeWalks`). Thread-local accesses are
  as 03.94 (their pages aren't lined up).
- Patch 0083: code caches 48, 40, 32 and 8 MiB (still 128 in all).
- The 30-second line: aliases so far, how many were put on a copy, how many couldn't be, the
  padding entries and how many arena maps used one (`PS4Perf::aliases*`,
  `Orbis::ArenaPaddingCount`, `ArenaPaddedMaps`); how many of the timer events (8,500 a second in
  03.94's races) were the kernel's timer, guest threads' sleeps and timeouts ending
  (`kernel_timer_tasks`); and each of the five busiest services' wait before its thread took the
  call (`request_queue_ns`, set where `KServerSession` takes the request, added to
  `ServiceCall::queue_ns`), to see which service the 80 us waits are in.
- `game/stacktest.c` (in `test.sh`): `svcMapMemory` of 1 to 64 pages at each place in a 16 KiB
  page and two aliases a page apart: the alias reads the source's data, its writes are back in the
  source after `svcUnmapMemory`, the pages beside it stay free to `svcQueryMemory`; then 40 rounds
  of 8 threads (libnx maps their stacks) recursing on their stacks. A desktop build with
  `-DEDEN_TEST_ARENA_LINE_UP=1` (the kernel's side alone, a padding table that only logs): 362
  aliases put on copies, every padding entry taken out again, the applet pool back to the same
  free size after each alias; all checks pass, and `test.sh` passes (0 failed).

03.94: **stack and thread-local accesses walk from the start once one is declined; code cache
use per core.** 03.93 on a console (Mario Kart, 50%, Profiler off): races at 22-24 fps (42-48 ms of
work a frame); a service call 160 us (80 until the service's thread took it, 50 handling, 25 back);
in a settled race the translator 60-100 blocks a second (12-16 ms). But fastmem declined 5,000
accesses a second while loading, 400-1,900 at a race's start, and each decline recompiles its block
(about 150 us with the fault), the old code left dead in the cache: two whole-cache flushes in the
session (60 s, 180 s), each followed by everything translated again.

- Patch 0082: each fastmem access's patch information says what its address is based on
  (`Optimization::ClassifyAddressBase`: the stack pointer, the thread pointer (`A32CoprocGetOneWord`,
  `A64GetTPIDRRO`), or other, through constant adds and subtracts). A decline of a stack or
  thread-pointer access sets the emitter's `stack_walks` / `thread_local_walks`; from then on every
  block compiled has all its accesses through that base go through `A32SharedWalkPass`, lone ones
  too (`min_members` 1), so they never fault first. Host harness, split stack, 50 functions with a
  PUSH/POP each: 1,000 fallbacks (faults) before, 20 now; results equal. Old build vs new, every
  option on: 1,206 programs (split stack, fastmem) equal.
- Patch 0082: `PS4Perf::jit_cache_free_kib[4]`, `jit_core_full_flushes[4]`: the 30-second line
  shows each emulated core's code cache free space and its whole flushes.

03.93: **declined accesses share a walk; the profile read.** 03.92 on a console, Mario Kart with
the Profiler on (resolution 100%): races at 19-20 fps; core 0 75% in JIT code, 9% the app's code, 16%
in system calls (12-13% idle); a service call 250 us (about 130 until the service's thread took it,
77 its handling, 38 back); the translator 15-40 blocks a second in races (120-190 us each),
thousands a second while loading. hotblocks.log: memset and memcpy loops on top, then NEON code;
in the 60 busiest blocks about 190 accesses walked the page table and 95 used fastmem. The walking
ones: PUSH/POP and stack-frame stores, thread-local storage reads (`mrc p15` then
`ldr [r0, #0x1fc]`), and the memset/memcpy stores. Fastmem declined about 240 accesses a second all
race long, nearly all "pages not back to back": the 4 KiB guest pages under a 16 KiB host page not
consecutive or not 16 KiB-aligned in the DRAM (thread stacks, local storage pages), so the arena
can't map them; each such access was recompiled to walk the page table on its own.

- Patch 0081: `fastmem_declined_shared_walk` (A32, on by default): the accesses fastmem was declined
  for (their `do_not_fastmem` markers: the block's location and the IR name, which NamingPass gives
  before any later pass changes the block) go through `A32SharedWalkPass` after Optimize, so those
  through one base register value plus constants (a PUSH or POP, a stack frame, a copy loop) share
  one `A32HostPointer` walk; every other access keeps fastmem. The emitter keeps the blocks with
  declined accesses (`blocks_without_fastmem`).
- Host harness `splitstack` (the stack's pages left out of the fastmem arena): calls.s (PUSH r4-r11,
  lr; VPUSH d8-d15; and the pops, a call): 55.2 ns a call per-access walks, 21.1 shared, 18.1
  fastmem, 24.5 without fastmem. Old build vs new, every option on: gen7, gen, gen3 (split stack),
  gen4 (fastmem): 1,208 programs equal.
- Patch 0081: the services on host threads (`RunHostThreadFunc`: audio, nvservices, FS, vi, bsdsocket,
  ldn) run at VeryHigh (the PS4's 640): at the default (700) a woken one waited behind the emulated
  cores' threads (650; the fourth spins 50 us when idle), the Vulkan thread and audio. The services
  on the emulated fourth core (hid, nvnflinger...) are unchanged.
- Profiler: the session line starts with the build (the sampler cut it at 300 characters, so
  03.92's own samples were taken for an earlier build's); the summary's shares come from each
  window's counts (the game's code was 7% of core 0 counting only addresses seen twice in a window).

03.92: **diagnostics after 21 fps: a service call's wait split up, translation time, one more
launcher trace.** 03.91 on a console: Mario Kart races at 21 fps (03.88: 16-19), about 500 s without
a problem; Captain Toad ran too (03.89/03.90 had closed it as Mario Kart, the same `jmp(qword)` in
A64's inline RSB pop). Its launcher trace (the 30-second lines) was gone: each game runs in an app
run of its own and the next run's start renamed its trace over it.

- `port/ps4_crashlog.cpp`: the launcher trace the app's start renames away is kept as
  `boot-trace-launcher-previous.log` (the game before the last one).
- Patch 0080: a synchronous call to a service is stamped when sent, when the service's thread takes
  it (`ReceiveRequest`) and when answered (`SendReply`); the 30-second line splits the average wait
  (03.91's Mario Kart race: about 200 us a call, about 50 of it the services' own handling).
- Patch 0080: the time dynarmic spends translating and emitting new blocks, per second.

03.91: **03.89's crash at Mario Kart's start fixed; one xbyak for the whole program.** 03.89 on a
console: Mario Kart closed while starting, `uncaught exception Xbyak::Error: bad mem size` on two
cores, thrown in `A32EmitX64::EmitLeafTerminal` (noexcept, so terminate). The inline RSB pop
(patch 0077) ended with `jmp(qword[...])`; xbyak's `jmp` builds its operand register as 32 bits
there, so its strict size check (`XBYAK_STRICT_CHECK_MEM_REG_SIZE`) rejects the qword. dynarmic
compiles with the check off (its CMakeLists: newer xbyak's strict checks fail in dynarmic), and so
did the host harness: never seen there. But `video_core/macro.cpp` and `renderer_null/software_draw.cpp`
include xbyak with the default (on), and the linker keeps one copy of each of xbyak's inline
functions for the whole program: the PS4 build (03.88 too) had the strict `opMR`.

- Patch 0079: the inline RSB pop jumps through `ptr[...]` (as the shared handlers always did).
- Patch 0079: those two video_core files get dynarmic's xbyak settings
  (`XBYAK_STRICT_CHECK_MEM_REG_SIZE=0`, `XBYAK_OLD_DISP_CHECK=1`): one behaviour for every copy.
- The host harness has a strict build now (`build-strict`, the check forced on), which reproduced
  the throw (with the inline RSB pop only). Fixed: the A32 and A64 benchmarks and 6,600 runs of
  every generator's programs, every option on, the PS4's CPU features, throw nothing; old build vs
  new, 242 A32 and 81 A64 programs equal.

03.90: **predicated A32 instructions in place; float compares straight to the flags.**

- Patch 0078, `inline_conditional_instructions` (A32, on in Eden): a conditional ARM instruction in
  the middle of a block is translated in place; each register write becomes `ConditionalSelect`
  (new value if the condition passes, else the value read before the instruction), and a write of
  all four flags (CMP, CMN, ADDS...) a `ConditionalSelectNZCV` against a new `A32GetCpsrNZCV`. One
  that touches memory, writes only some flags (TST, MOVS), branches, raises, or does floating
  point or vector work has its IR undone and ends the block as before. It used to split the
  code in three blocks (before, a conditional block, the rest) with links between them.
- Patch 0078, `A32FpscrNZCVForwarding`: `VMRS APSR_nzcv` after a `VCMP` in the same block
  stores the compare's flags (host format) straight to the CPSR, instead of through the FPSCR's
  ARM format and back.
- Patch 0078, `inline_fast_dispatch` (A32 and A64, on in Eden): an indirect jump
  (`FastDispatchHint`: BLX/BLR through a register, jump tables) hashes into the fast dispatch
  table and jumps in its own block, its miss path shared, instead of in the one handler all of
  them share. Two alternating single-target call sites, 100,000 calls (callgrind's branch
  simulator): indirect mispredicts 106,401 -> 6,405.
- Host harness: A32 459k -> 449k host instructions a frame with both (game.c has few predicated
  instructions; 03.88: 632k). gen7.py now also makes conditional compares, conditional VFP moves
  and adds, and float compares (VCMP/VCMPE, with zero, double) with VMRS before branches and
  conditional instructions, indirect calls; old build vs new with every option on, with and
  without fastmem: 1,202 gen7 + 451 gen8 programs equal (and 3,016 A32 programs in the rounds
  before); a generator bug (Thumb indirect calls into ARM state) failed 103 programs on both
  builds alike, and those seeds pass once fixed.

03.89: **leaner translated code; a profiler that names the game code.** The profile every log
since 03.77 carried was one MK8 session recorded before 03.77, symbolized with each newer build's
functions (its "hot" A32 translator functions were nonsense). Its useful part: MK8 Deluxe is A32,
and in races core 0 spent 80-85% in JIT code. So the A32 output was taken apart on the host
harness (a game-like image, the PS4's CPU features, instruction counts under callgrind).

- Patch 0077, A32: `B<c>` ends its block with an `If` terminal (Thumb's already did) instead of
  ending the block before it for a conditional block of its own, two linked blocks per loop turn.
- Patch 0077, register allocator: a value in the way moves to an empty register (callee-saved
  first; only those during a host call), not to the stack and back. It was what `UseScratch` of a
  value used again, and every fixed-register scratch (`lahf`'s rax, shifts' rcx), cost.
- Patch 0077, options Eden turns on: no per-block `%rbp` frame save/setup/restore (three
  instructions a block, for host frame-pointer walks); no halt check on links to a higher address
  than the block's start (every loop's way back still checks); each return checks the return
  stack buffer and jumps in its own block (one shared indirect jump mispredicts whenever
  consecutive returns differ).
- Host harness, 200 frames: A32 632k -> 472k host instructions a frame (-25%), A64 513k -> 447k
  (-13%). New random-program generators for control flow and flags (gen7.py A32/Thumb with
  conditional instructions, IT blocks, loops, calls; gen8.py A64 with CSEL/CCMP/CBZ/TBZ, loops,
  calls); old build vs new, every option on, with and without fastmem: 4,808 programs, all equal.
- Profiler: the session line carries the build (`[build 03.89]`); a samples.log from another build
  is summed up without names. JIT samples name their guest block (dynarmic's block map, kept while
  the profiler is on), the summary lists the busiest blocks per thread, and every minute
  hotblocks.log gets the 60 busiest blocks' guest words and host bytes (for disassembly).

03.88: **a fresh process for every game; faults inside fault handlers reported.** 03.87 on a console:
Mario Kart 8 Deluxe races at 16-19 fps (mostly 17.5-18.7; 03.85 15-18, 03.86 14-16), accesses through
the callbacks 9,000-32,000 a second, nearly all writes to memory the GPU caches (03.86: 240,000-350,000),
fastmem resolving about 300 faults a second and declining 8-34. Then Captain Toad, booted after it in
the same session, was closed by the system while loading a level, no crash report, its GPU submissions
all completing. Its start: `C heap 72 of 1116 MiB in use, 37 MiB of flexible memory free, largest free
block of direct memory 1856 MiB` (a fresh app: 384 MiB, 364, 4608): the C heap keeps what a game took,
much of it in direct memory. And fastmem declined 1,475 faults a second in it.

- The launcher restarts the app after a game that ran (`sceSystemServiceLoadExec("/app0/eboot.bin")`;
  if refused, it carries on as before): every game starts with all the memory there is.
- Patch 0076: the fault handlers (dynarmic's on the PS4, the on-demand DRAM's, the sparse vectors')
  run with SA_NODEFER and share a per-thread nesting count; a fault while one runs is passed down to the
  crash logger untouched (resolving it could wait on a lock its own thread holds), so crash.log says
  where, instead of the kernel ending the app as it does for a fault on a blocked signal.
- The 30-second line splits fastmem's declines by reason: not plain memory, writes to memory the GPU
  caches, 16 KiB pages whose guest pages aren't back to back in the DRAM, other.

03.87: **exclusives that lose fastmem stay inline; GPU-cached pages readable through it.** 03.86 on a
console, Mario Kart 8 Deluxe: fastmem in use (a 2^39 arena at 0x4800000000, 1.7-3.7 GiB mapped, tens
of faults a second resolved, 60-320 declined), races at 14-16 fps (03.85: 15-18), and 240,000-350,000
accesses a second through the callbacks, half of them writes: 03.79's exclusive pairs again. Each
declined fault recompiles its access without fastmem; for a load or store that's the inline page table
walk, but dynarmic's fastmem exclusives then called the callbacks every time, and 03.80's page table
exclusives were off whenever fastmem was on.

Patch 0075: an exclusive recompiled without fastmem goes through the page table inline
(`EmitExclusive*PageTable`, the monitor's state shared with fastmem's), and `UsePageTableExclusives` no
longer needs fastmem off. The fault's error code (FreeBSD's mc_err, bit 1) reaches the resolver, which
maps a 16 KiB page mixing ordinary memory and pages the GPU caches read-only when such pages may be read
without the callbacks (no reactive flushing, as the page table walk), and declines a write there.

The A32 harness gained fastmem too (memfd guest memory in a 4 GiB arena, the marked and unmapped pages
left out). 2,100 random programs agree with the page table (A32/Thumb loads, stores, LDM/STM and
exclusives; A64 loads, stores and exclusives). A32, fastmem against the page table with its shared walk
and inline exclusives: calls 25.5 -> 24.4 ns, push/pop 27.8 -> 20.6, struct accesses 5.5 -> 3.8,
atomics 78.8 -> 74.4.

03.86: **fastmem in use.** 03.85 on a console: no more crash, and the 30-second line said `fastmem (a 2^38
arena at 0x8000000000): 0 MiB ... mapped, 0 faults`: never used. With Unsafe CPU accuracy Eden drops the
fastmem pointer unless `cpuopt_unsafe_host_mmu` is set, whose default is off on platforms it doesn't
list (the PS4); the launcher sets it (`main.cpp`). The 32-bit-only optimisations still running (they
need no fastmem pointer) confirmed it.

Patch 0074:

- the arena is tried at 288 GiB first (the GPU windows take 256-288 GiB; the kernel places other
  mappings from 64 GiB up), where 2^39 can fit; 03.85 got only 2^38 above 512 GiB;
- A64, an access past the arena (a 39-bit game with a 2^38 arena): the page table walk inline,
  deferred (`EmitA64FastmemRangeWalk`), the fallback only if it fails, not the callbacks every time;
- A64, the range check: `cmp vaddr, [limit]` / `jae` (the limit in the constant pool), not a copy, a
  shift and a scratch register;
- `A64SharedWalkPass` with fastmem too: a group through one base walks once, a lone access uses
  fastmem (both reach the same memory).

The A64 harness gained guest memory in a memfd, mapped into a 2^32 arena at the guest's addresses
(but the marked and unmapped pages, which fault: the fallback, and the code recompiled without
fastmem there), and a 2^28 arena (every access past it). Four dependent loads 15.3-18.6 -> 7.4-7.9
ns; a call with a prologue, an epilogue and struct accesses 11.9 -> 11.1 ns; atomics level. 700 random
load/store and 500 exclusive programs give the same registers, vectors and memory in both modes as
through the page table.

03.85: **the fastmem arena above the GPU's addresses.** 03.84 on a console: with fastmem on, Celeste
and Captain Toad were closed by the system at their first frame, no crash report; with it off they ran.
The logs: the arena was reserved at 0x1118200000 (512 GiB, 68 to 580 GiB), and the port's Mesa, which
reserves its GPU address windows from 256 GiB up, got 0x9164a00000 and 0x9600000000 instead of
0x4000000000 and 0x4400000000; its first submission never completed (`submit 1 ... GPU completed 0`,
then nothing): a GPU fault, which the system answers by closing the app, with no CPU signal. The arena
(CPU only) is now reserved at 1 TiB or above, else 512 GiB or above (`ReserveAboveGpu`), never lower,
so the GPU's windows stay at 256 GiB; without such a place, no fastmem. The 30-second line gives the
arena's size and address.

03.84: **fastmem.** 03.83's probe on a console: `plainly: refused (0x80020010); after
sceKernelEnableDmemAliasing (0x0): yes, the same memory at two addresses`. Its log of Mario Kart 8
Deluxe (races at 15-17 fps, Frame Pacing Unlocked changing nothing): emulated core 0 busy 85% of the
time, cores never all idle at once, frames not waiting for vsyncs, services cheap (audio's
RequestUpdate 189 a second at 90 us the most): the game's own code.

Patch 0072:

- `common/orbis_memory.cpp`: `EnableAliasing` (sceKernelEnableDmemAliasing, once),
  `ReserveFastmemArena` (the largest of 2^39 down to 2^32 the kernel gives), `InvalidateArena`
  (mprotect to nothing, 16 KiB pages), an arena lock, and `ResolveFault`, one resolver for every
  lazily mapped range: a sparse vector's page (the page tables), an on-demand DRAM chunk, and an arena
  page, mapped from the application's page table when its four 4 KiB guest pages are ordinary memory
  back to back in the DRAM (`sceKernelMapDirectMemory`, MAP_FIXED, the DRAM chunk's direct memory,
  committing it if need be), up to 512 KiB at a time. Anything else is declined.
- `common/host_memory.cpp`: the arena reserved after the DRAM unless `EDEN_PS4_FASTMEM=0`; Map and
  Unmap invalidate, Protect does when it takes rights away (the arena maps read/write, as the page
  table walk treats memory); released before the DRAM. `Settings::IsFastmemEnabled` is "an arena of
  2^32 or more".
- `core/memory.cpp`: Map, Unmap, MarkRegionDebug and RasterizerMarkRegionCached hold the arena lock
  across the page table change and the invalidation (`Orbis::ArenaUpdate`), so no fault maps a page
  from entries about to change (a page the GPU starts caching mapped read/write would lose writes).
- dynarmic: on the PS4 the POSIX exception handler, with the PS4's (FreeBSD's) signal context (the
  machine context at 0x40, RIP and RSP at 20 and 23: measured by the port's crash logger), asking
  `dynarmic_ps4_resolve_fault` first, treating only faults inside the arena as fastmem accesses (the
  fallback, and the code recompiled without fastmem there), chaining the rest, logging nothing.
  A64's `fastmem_address_space_bits` is held to the arena's.

System Settings, CPU, Fastmem (on by default; `fastmem=` in the menu file). The 30-second line adds the
MiB mapped in the arena and its faults resolved and declined.

03.83: **page table entries with the whole offset, and a probe for direct memory aliasing.**

Patch 0071: on the PS4 (`EDEN_PAGE_ENTRY_FULL_OFFSET`, `common/page_table.h`) an entry keeps the host
offset whole in bits 12-63 (the block id keeps its low 9 bits: it's only ever read back to be stored
again, `core/memory.cpp`), so `ATTRIBUTE_MASK` is `~0xFFF` and the recompilers get no sign extension:
their walk is `and page, -4096` instead of that plus `shl`/`sar` by 8, on every load and store not in a
shared-walk group (2 instructions fewer, 2 cycles fewer from the entry's load to the access). A packing
test (2,000,000 random entries, negative offsets, marked bits, block ids) and the harnesses built both
ways (the same random A32, Thumb and A64 programs, loads and stores and exclusives, 1,400 of them) give
identical results; four dependent A64 loads 19.6-20.8 ns -> 16.0-16.5 ns.

The boot trace's double-mapping probe (2 MiB, the on-demand DRAM's chunk) now also tries libkernel's
`sceKernelEnableDmemAliasing` and `sceKernelEnableDmemAliasing2` (exported by `libkernel.sprx`, absent
from the SDK's headers; a `libkernel_dmem_aliasing2_for_dev` variant exists too) before mapping again,
and `sceKernelMapDirectMemory2`, each read/write test under a temporary SIGSEGV/SIGBUS guard. Fastmem
needs exactly this: the Switch's memory at its own addresses beside Eden's view of it.

03.82: **Game Speed: 1.0x, 1.5x, 2.0x** (System Settings, Graphics; `speed=` in the menu file,
`EDEN_PS4_SPEED` on the desktop). Eden's speed limit at 100, 150 or 200%: its conductor's vsyncs come
that much more often (`vi/conductor.cpp`), so a game that reaches its full frame rate runs that much
faster, a game stepping by frames faster in game time, one stepping by the clock only smoother (the
guest's CNTPCT isn't scaled; `sync_core_speed` stays off). Each composite waits for the one before it
(`HardwareComposer::ComposeLocked`), and the PS4's display flips at the TV's vsync, which would cap it at
60 again: above 100% `vsync_mode` is Immediate, so `vk_swapchain_ps4.cpp` flips at the next scanline
(`ORBIS_VIDEO_OUT_FLIP_HSYNC`), tearing possible. It can't make a CPU-bound scene faster. Frame Pacing:
Unlocked (no speed limit) overrides it.

03.81: **one page table walk for A64 loads and stores sharing a base.** 03.80's log of Captain Toad:
Treasure Tracker (64-bit) at 15 fps: emulated core 0 busy all the time, all cores idle at once 0-1%,
finished frames waiting 8 ms for a vsync but the game never waiting for a buffer, so its main thread's
own code is the limit (Frame Pacing can't help it); 100-600 slow memory accesses a second (03.80's
inline exclusives at work in A64 too). Each A64 load and store walked the page table inline: a
misalignment test, the index, the 39-bit range test, the entry, mask, sign extension, a branch (about
12 instructions). LDP/STP were already one 128-bit access each.

Patch 0069: `A64SharedWalkPass`, the A64 version of 03.79's A32 pass, with `A64HostPointer` (the range
test included) and `A64ReadMemoryVia*` / `A64WriteMemoryVia*` (8 to 128 bits, XMM values for 128):
loads and stores through one base register value plus constants within a block (at most 256 bytes;
not acquire/release) share one walk, each then a test, a branch and one host load or store, and its own
walk and the fallbacks when the shared walk failed. The base is followed through chains of constant
adds and subtracts, so a prologue's pre-indexed `stp x29, x30, [sp, #-N]!`, the stores after it on the
new SP and the epilogue's loads are one group. In the A64 harness: a call with a prologue, an epilogue
and seven struct accesses 27-28.5 ns -> 11.7-12.1 ns. 1,500 random A64 programs (every size, LDUR/STUR,
signed loads, LDP/STP of W, X, D and Q, pre- and post-index, register offsets, SIMD, prologues on SP, a
load into its own base, a store of the base itself; at page boundaries, on the marked page, next to
and on the unmapped one, outside the address space) give the same registers, vectors, SP and memory
with and without it; the A32 and exclusive suites still pass.

Patch 0070: idle time counted up to now for a core still idle (a core asleep for minutes showed 0%,
then 488%), and the per-command service table emptied each period (it had filled with the services a
game uses only at its start, the busiest then showing as "others").

03.80: **exclusive loads and stores inline, a cheaper DMB, and where the idle time goes.** 03.79's
log (Mario Kart 8 Deluxe, 15-18 fps in races): 370,000 memory accesses a second left the recompiled
code, half of them writes. Without fastmem, every exclusive load and store (LDREX/STREX and sizes,
LDXR/STXR and sizes) was a call into the global monitor and the memory callbacks, its spinlock taken even
with `Unsafe_IgnoreGlobalMonitor` (which only the inline fastmem path honours): an atomic's load and its
store were a slow read and a slow write each. Its profile (the Profiler on): the emulated cores ran the
game's code 57-66% of the time and slept 27-38%, the GPU thread was busy 39%; no thread was saturated.

Patch 0067:

- `EmitExclusiveReadMemoryPageTable` / `EmitExclusiveWriteMemoryPageTable` (shared by A32 and A64): as
  the fastmem inline path, the monitor's address and value stored and the load read inline, the store a
  `lock cmpxchg` against the value loaded, but the host address from the page table walk; an access the
  walk can't do (unmapped, across a page, a store to a marked page) takes the same fallbacks. 128-bit
  exclusives keep the monitor path. `UserConfig::page_table_inline_exclusives`, on by default, with a page
  table and without fastmem.
- DMB: `lock or dword [rsp], 0` instead of `mfence` (a full barrier for ordinary memory, as Linux's
  `smp_mb` on x86-64; 7-9 ns against 17 on the build machine). DSB keeps `mfence; lfence`.

In the host harness (now also an A64 one, `dynbench64`, with Eden's packed entries and block ids above
511): an atomic increment and decrement with barriers 316-423 ns -> 79-98 ns (A32), 360-450 -> 53-68 ns
(A64). Random programs of exclusives (every size, acquire/release forms, pairs, CLREX, a store with no
load, to another granule, a plain store between, twice; on ordinary, marked and unmapped pages) give the
same registers and memory inline as through the monitor: 2,800 A32/Thumb, 1,040 A64. The earlier suites
still pass.

Patch 0068, for the 30-second line: each emulated core's idle time and the time all four were idle at
once; how long a finished frame waited for the vsync that showed it (QueueBuffer to the composer's
acquire); the game's DequeueBuffer calls and their waits; and the HLE services' own handling time per
service and command, the five busiest. These separate the remaining causes: the game's own code, a
global wait (a vsync, the GPU thread), or a service.

System Settings, Graphics, Frame Pacing (`pacing=` in the menu file; `EDEN_PS4_UNLOCKED=1` on the
desktop): 60 Hz is Eden's speed limit at 100%, its composer showing a frame at the next 60 Hz vsync, so a
game double-buffered at 50 ms a frame shows 15 fps; Unlocked turns the speed limit off (Eden's vsyncs 100
times as often), each frame shown when it's done. Races at 15-18 fps with every thread idle a third of the
time fit frames waiting for vsyncs; the 30-second line now measures it.

03.79: **one page table walk for many accesses, and pinned threads.** 03.78's counters (Mario Kart
8 Deluxe at 13-14 fps in races): idle cores woken in 14 us, service calls 130 us each 470 times a
second, timers on time, no code cache flushes, 330,000 slow memory accesses a second. What's left is the
game's own code. Stage 1 had measured why on a console: translated code is about 5x slower than
native, calls 10x. Mario Kart 8 Deluxe is 32-bit ARM code, and without fastmem every load and store
walks the page table inline (about 13 instructions); `PUSH {r4-r11, lr}` with `VPUSH {d8-d15}` is 17
stores and as many walks, the POPs 17 more. A host harness (dynarmic built on its own, the PS4's page
table configuration: absolute offsets, Eden's packed entries, sign extension from bit 55, shuffled host
pages, a marked and an unmapped page) measured it: a call with that prologue and epilogue 71-124 ns,
without them 6.

Patch 0065: `A32HostPointer` walks once for a span (the host address of its start, or 0 when it crosses
into the next page, the page is unmapped, or it's marked for a write), and `A32ReadMemoryVia*` /
`A32WriteMemoryVia*` use it: test, branch, one host load or store; their out-of-line slow path is the
access's own walk, then the callbacks. The translator uses them for LDM, STM, PUSH, POP, VLDM, VSTM,
VPUSH, VPOP and a double's VLDR/VSTR, and `A32SharedWalkPass` groups other loads and stores through one
base register value plus constants within a block (at most 256 bytes). In the harness: calls 32-42 ns,
push/pop 26-36 (from 68-86), struct accesses 8.4-9.6 (from 14-22), a page-crossing push/pop 66 (75).
Random ARM and Thumb programs (LDM/STM in every form, PUSH/POP, VPUSH/VPOP, VLDM/VSTM, VLDR/VSTR, LDR/STR
of every size through shared bases, at page boundaries, on the marked page and next to the unmapped one)
give the same registers and memory as before, thousands of them.

Patch 0066 and `port/ps4_runtime.cpp` (`ps4_place_thread`): CPUCore_0-2 on cores 0-2, the GPU thread on
core 3 (one Jaguar module, one L2), Eden's other named threads on the second module; System Settings,
CPU, Thread Placement: Free leaves them all on any core. The boot trace now says whether the PS4 maps the
same direct memory twice (`ProbeDoubleMapping`), what fastmem would need, and the 30-second line splits
slow memory accesses into writes.

03.78: **where Mario Kart 8 Deluxe's race frames go, and the first fixes.** Its races ran at 12 fps on
a console, its menus at 50-60. The boot trace's new GPU line (03.77) settled the GPU: busy 2-5% of the
time, and the CPU waited for it 5%. The profile: in races the emulated cores ran the game's code 49-59%
of the time and slept 26-45% (all of it the condition variable wait in `PhysicalCore::Idle`), Eden's GPU
thread was busy 35% (command processing, about 29 ms a frame, which the game waits for: with Low GPU
accuracy a syncpoint is signalled when the GPU thread reaches it), and no thread was busy all the time:
time lost to handoffs. Patch 0064:

- an idle emulated core spins up to 50 us before it sleeps, so work handed to it (a thread another core
  made ready, a service's reply) doesn't wait for a sleeping host thread to be woken;
- the services that run on host threads (nvdrv, vi, audio, FS: every GPU submission and sync wait is
  a call to one) spin the same way before they sleep waiting for the next call;
- the timing thread learns how much its timed waits overshoot (the PS4's kernel timers) and waits that
  much less, at most 1 ms, spinning the rest, so the game's timers, vsync and audio fire on time;
- dynarmic's A32 `DecodeVFP` and `DecodeASIMD`, which every ordinary ARM instruction went through first
  (10-15% of an emulated core whenever code is translated), reject encodings outside their spaces with
  one test (checked against every pattern);
- the boot trace's 30-second line says, from `common/ps4_perf.h`: idle cores woken while spinning or
  after sleeping and the wake-up's delay, the game's service calls (`svcSendSyncRequest`) and their
  round trips, timer events' lateness and the learned overshoot, the recompiler's translations and
  cache flushes, and memory accesses that left its inline page table walk.

The Profiler (on in that run) interrupts the profiled threads 1000 times a second with a signal: off
for playing. On a PS4 Pro, the system's Boost Mode runs the CPU at 2.13 GHz instead of 1.6.

03.75: **Game Folder**, after Crema's. Crema finds games anywhere because the player chooses
where: its Game Folder setting browses from a list of places (the data folder, `/mnt/usb0-7`,
`/mnt/ext0`, `/mnt/ext1`, `/mnt`, `/`) and the folder chosen is searched through. Games and Keys
now has the same (Triangle): a browser over the same places (`HomeMenu::OpenFolder`; "Use This
Folder", "..", the folders in it, "No Game Folder"), the choice kept in `menu.txt`
(`games_folder=`). The folder is searched six folders deep for games (listed as "Game Folder",
before the drives', a file found twice shown once), and for `prod.keys`, `title.keys` and the
firmware as a drive is (03.74), before the drives; its entries join the 3-second check, so a game
added there shows. A drive the launcher doesn't look at by itself (mounted somewhere other than
`/mnt/usbN`) can be reached this way. `ui-preview.sh` draws the browser.

03.74: **keys and firmware used from the USB drive.** `prod.keys`, `title.keys` and the firmware on
a USB drive no longer need importing: before each boot the launcher looks for them on
`/mnt/usb0-7` as Import does (`Launcher::FindKeysAndFirmware`: `prod.keys` at a drive's top, in a
folder there such as `switch/`, or that folder's `keys/`; the firmware the folder, at most two
down, with the most `.nca` files) and has Eden read them where they are
(`Launcher::SetKeysAndFirmware`): its keys folder is set to the drive's, and the keys are read
again at every boot (they were read once per app session), and patch 0062 has the system NAND's
registered content, the firmware, come from a folder the frontend names
(`Settings::values.system_contents_dir`, read only) instead of `nand/system/Contents/registered`.
What's on a drive comes before the user folder's; without a drive, the user folder's as before.
Saves and everything else Eden writes stay in the user folder. A firmware still in its `.zip`
can't be read in place: Import unpacks it as before. Games and Keys and the boot trace say which
keys and firmware games use.

03.76: **logs without a network, and the game's speed.** Celeste boots on a console (keys and
firmware used from the drive, 03.74/03.75). The log bundle (the web page's "Download all logs":
code addresses named, samples.log summed up) is now written as `eden-ps4-logs.txt` into the Game
Folder as the app starts (the last session's) and after each program, for a console kept off the
network; `WriteLogBundle` in `src/web_server.cpp`, shared with the page. While a program runs, Boot
logs Eden's own measure every 10 s: `speed: the game N fps, emulation at N% of full speed, N ms of
work a frame` (`System::GetAndResetPerfStats`), in log.txt and the boot trace. In shadPS4: Breakout
at 46-48 fps, 100%, and the file written to its USB drive at start and after the game.

03.77: **an FPS counter, and copying games to the PS4.** Celeste plays smoothly on a console from
the drive (03.75's Game Folder) once its first start (shaders, JIT) is over. System Settings,
Graphics, **FPS Counter** draws the game's frame rate in the top left corner: patch 0063
(`show_fps_overlay`, `window_adapt_pass.cpp`) counts the frames whose image changed in each second
(a game presenting one image twice counts once, so it matches Eden's own measure), drawn as
seven-segment digits with two `vkCmdClearAttachments` lists in the present pass, nothing new to
compile. A game's Options (from the Game Folder, a USB drive or the import folder) now offers
**Copy to the PS4**: copied to the data folder `/data/eden-ps4` in 4 MiB pieces through a `.part` name, on its
own thread with its progress on screen, only if the PS4's drive keeps 1 GB spare afterwards. The
list shows a game once, from the PS4's drive, when both have it.

03.73: **USB drives.** On a console the app listed nothing from an exFAT drive the package had been
installed from, though Crema, on the same runtime, lists Wii U games from USB drives on the same
console. Three things in the launcher's own scan, none a matter of permissions: the game list was
made once, as the app started, so a drive the system mounted after that (or plugged in later) never
showed; only a drive's top was searched, not its folders; and Import from USB wanted `prod.keys`
in exactly that case. Now the drives are checked every 3 s and the list is made again when one
changes, games are found at the top and one folder down (system and hidden folders left out), with
`opendir`/`stat` as Crema-style scans do, and the keys' names match in any case. Games and Keys (and
the boot trace) say what the app sees on each drive, down to the names at its top. In shadPS4 (its
`/mnt/usb0` is `user/mnt/usb0`): a drive filled after the start page was up showed its game from
`switch/` within 3 s, and Import from USB took `Prod.keys` and the firmware zip from a folder.

03.71: **firmware zips.** Firmware usually comes as a `.zip` of its `.nca` files; the web page now
takes it as it is and the PS4 unpacks it (`src/firmware_zip.cpp`, with zlib from the sysroot): the
zip's central directory, stored and deflated entries, ZIP64, `.nca` files from any folder in it,
"archive bit" folders (`x.nca/00`, `01`...) joined into one file, every file checked against its
CRC, and nothing written outside the firmware folder whatever the zip's paths say. It unpacks
beside the firmware there and swaps it in only when every file came out right, so a damaged
download leaves the old firmware working. Import from USB does the same with a firmware zip on the
drive when no drive has it unpacked. Tested on the desktop with zips Python made (stored, deflated,
ZIP64, archive bit, `../` paths, a damaged one, one with 3 files: refused as no firmware), through
the server, its page in Chromium, and the USB import.

Found testing it in shadPS4: `std::filesystem::remove_all` has never removed anything on the PS4.
libc++ walks the tree with `unlinkat`, which the PS4's C library has only as a stub that fails with
ENOSYS. So the firmware a USB import (03.40) or the web page (03.60) replaced was never cleared: a
second firmware was mixed into the first. The launcher removes folders entry by entry now
(`RemoveFolder`, with `unlink` and `rmdir`, which the system has), and so does Eden's
`RemoveDirRecursively` on the PS4 (patch 0061: a game deleting a save folder, Eden clearing its
temporary storage).

03.70: **the Switch's RAM on demand, so the GPU gets the memory games don't use.** Eden mapped
the Switch's 4 GiB of DRAM onto 4 GiB of the PS4's 4.5 GiB of direct memory as a program started,
leaving about 480 MiB for RADV (textures, buffers, command buffers) and the C heap's overflow;
Eden4 lists running out of it as the way large games fail. Now (patch 0058, `common/orbis_memory`)
the DRAM range is only reserved, and a fault handler maps a 2 MiB chunk of direct memory the first
time anything touches it: zeroed at an address the kernel picks first, then mapped in place (mapped
first and zeroed there, another thread touching the same chunk could see it half cleared or have
its write wiped: a desktop test of the mechanism against a fake of the PS4's direct memory caught
exactly that, and runs 16 threads onto fresh chunks 50 times now). A zero fill of chunks never
touched, as a new heap gets, leaves them alone, so a game's heap takes memory only as it's used.
Direct memory run out makes the fault wait up to 4 s for the GPU's caches to give some back before
it is a crash. `IOFile` touches a large buffer before the C library passes it to `read()` or
`write()`, as the kernel's own accesses don't fault into the handler (they fail with EFAULT).
System Settings, System, Switch Memory (`EDEN_PS4_DRAM_ON_DEMAND=0`) maps it all up front as before.

The texture and buffer caches' garbage collection follows: on the PS4 their memory use is what the
GPU's buffers hold (the winsys's own count, `ac_ps4_gpu_bytes`), scaled against the free direct
memory plus that, less 384 MiB kept for the DRAM to grow into. As a game's RAM grows, the GPU's share
shrinks and the caches collect harder, handing direct memory back. dynarmic's POSIX fault handler is
no longer built for the PS4: with no fastmem it registered no code, and logged every fault (from the
signal handler) before passing it on to the page table's and now the DRAM's handlers.

Bug found on the way (patch 0060): a file no loader takes (one that isn't there, or not a game)
left the kernel initialized, and destroying the system after it crashed (AddressSanitizer: the
kernel's object list still held objects the slab heaps had freed). Every other failed load shut the
kernel down first; this one does too now, and `test.sh` checks both kinds of failure.

Updates and DLC: `.nsp` files in `/data/eden-ps4/updates`, sent from the web page's new section or
copied there, are applied to their game as it boots (Eden's external content, nothing installed).
Every 30 s while a program runs, the boot trace has a line with its frame rate and the memory: the
Switch's RAM in use (and its peak), the GPU's buffers, the C heap, and the largest free block of
direct memory, which says how much a game really needs on the console.

03.60: **files and logs over the network, and readable logs.** The app runs a small web server
(`src/web_server.cpp`, port 8080; `EDEN_PS4_WEB_PORT` changes it) and shows its address at the top
of the HOME menu. From a browser on the same network, its page uploads `prod.keys`/`title.keys`,
a firmware (a whole folder of `.nca` files at once, replacing the one there) and games, streamed to
disk with progress (a 1.5 GB game in 8 s on the desktop), and downloads every log as one file.
It has no password: anyone on the network can reach it while the app is open (it never sends the
keys back, only logs). The calls it makes (`socket`, `bind`, `listen`, `accept`) are libkernel's,
loaded in every app; in shadPS4 the page, the uploads and the log bundle all worked.

The package now carries `symbols.zst`: the ELF's function names (`build.sh`, from `llvm-nm`), 0.75
MB compressed. The log bundle names every code address in `crash.log`, `hang.log`, `samples.log`
and the boot trace's `callers` lists, and the error screen after a crash shows the functions on the
crashed thread's stack: no ELF of the exact build is needed to read them any more
(`src/log_tools.cpp`). While a program runs the bundle skips the names (they take about 11 MiB of
memory to load) and sends at most each log's last 2 MiB.

System Settings, CPU, **Profiler** turns on Crema's sampler (`port/ps4_sampler.cpp`) while a
program runs: 1000 times a second it records where the emulated CPU cores (`CPUCore_*`), the GPU
thread and `VulkanWorker` are, and every 10 s writes the counts to `samples.log`, with a `## `
line naming each program. The web page's Profile summary (`/api/profile`) sums them up per program
and thread: the share in the game's translated code (JIT), waiting in system calls, in other system
library code and in the app's own code, and its busiest functions by name. For Eden, which runs one
program after another, the sampler now takes threads by name (`ps4_sampler_sample_threads`), and a
thread that ends leaves its slots in the sampler and the hang dump's list (`ps4_thread_ended`), so
the next program's threads take them; each window's header also says how many samples were in
system calls (in the kernel, nearly always waiting).

03.51: **every program that plays sound was killed on a console** as it opened its audio output:
the launcher never loaded the AudioOut system module (Crema's list has it), and a PS4 kills an app
that calls into a library whose module isn't loaded, without a signal, so without a crash log.
shadPS4 doesn't care. Breakout (03.30) and Blocks (03.50) died that way, a few seconds in; the
controller and GPU tests, silent, ran. The boot trace now also says when the heap has to grow, for
what size, on which thread and from where (code addresses: symbolize them with the build's ELF).
The first console runs grew it past 540 MiB in a test program, where the desktop and shadPS4 use a
fraction of that.

Bugs found in 03.50 (an AddressSanitizer build of the desktop launcher, `test.sh` under load):
- **A program's exit could free its GPU address space under the GPU thread** (patch 0050). The
  address space device owned its memory manager alone and channels had a raw pointer; a program
  that closes it right after its last submission (as it exits) freed it while the channel's last
  commands still ran. The test program's 3D stress test crashed at about one exit in three under
  load (a core dump showed the freed object's table pointer read back). Channels share it now.
- **Input after shutdown reached a freed stack** (patch 0056, launcher). 03.40's fix for the
  threads' fibers freed them at the kernel's shutdown, but the AM service's button poller lives on
  its service thread's stack (which never unwinds) and keeps a callback in the controllers, which
  outlive that; the controller thread, still polling, ran into it. The fibers go with the kernel
  now, after everything else, and the launcher stops feeding input before shutting down.
- **Every Vulkan shutdown wrote to freed memory** (patch 0057): the GPU's members went in reverse
  order, the channels (whose 3D engines' dirty flags the Vulkan state tracker points to) before the
  renderer, whose last submission then marked those flags. The renderer goes first now.
- **The network's interrupt pipe** (patch 0049) kept its closed descriptors' numbers after
  `Finalize`; a later `pipe()` failing (shadPS4 asserted at every shutdown) left them, by then
  other files', to be written to.
- `log.txt` stops at 32 MiB (a game that warns every frame would fill the storage over a session).

If Vulkan doesn't start (RADV can't open the GPU), the launcher keeps that boot's log as
`log-vulkan.txt` and boots the program again with the software renderer, which it then keeps
using until it is restarted. RADV's own log is in `/data/eden-ps4/gpu/radv-ps4.log` (with
`gpu-hang.txt` and `gpu-fault.txt` there when the GPU hangs or faults).

Status (03.00): Mesa 26.0.8 builds with Crema's patches, and the package links it (59 MB). In
shadPS4 the launcher gets as far as creating the Vulkan instance and then falls back: shadPS4 has
no `libSceGnmDriver.sprx` for the winsys to load (and it emulates the GPU from PSSL shaders, not
the raw GCN code RADV writes), so RADV can only be tried on a console. To report how it went
there, send `log-vulkan.txt` (or `log.txt`), `gpu/radv-ps4.log` and `crash.log` from
`/data/eden-ps4`. Patch 0044 fixes a crash in Eden when a boot fails (the new process was
destroyed after the kernel), which the fallback needs.

03.10 takes what Eden4's test builds learned on consoles (its `docs/CHANGELOG.md` and
`KNOWN_ISSUES.md`) where this port hadn't yet: the app heap's overflow leaves the GPU its direct
memory (Eden4 00.04 and 00.15: Vulkan out of memory), Eden's busy threads get heap arenas, and a
hang watchdog writes `hang.log` (both below). The package already runs in base PS4 mode (PS4 Pro
mode hung the GPU and crashed the console for Eden4: the winsys describes the base GPU), and the
heap's locks were already blocking ones (a spinning lock deadlocked Eden4 00.15 and Crema 1.61). In
shadPS4: the heap test grew the heap 10 steps into flexible memory and 7 into direct memory, then
stopped with 262 MiB of direct memory free; a program that stops presenting got a `hang.log` with
each of Eden's threads by name (the emulated cores idle, the GPU thread waiting for work). Handing
the screen to the GPU display now waits for VideoOut's last flip however long it takes (up to 2
s): one still queued after 100 ms took shadPS4 down.

03.12 fixes the crash Eden4 lists for running out of GPU memory in RADV itself: when a command
buffer's next IB couldn't be allocated, Mesa's error path wrote a chain packet before the old
IB's start and kept recording past its end. It now records into a host buffer that is thrown
away, and the command buffer fails with `VK_ERROR_OUT_OF_DEVICE_MEMORY`
(`../patches/mesa-ps4-cs-grow-oom.patch`).

## What's next: the GPU

What draws with the CPU runs now; what draws with the GPU (every commercial game, and homebrew
built on deko3d, OpenGL or SDL's renderer) needs the Switch's GPU emulated. Eden turns the
Maxwell GPU's commands and shaders into Vulkan or OpenGL, and a PS4 app has neither: its GPU
speaks GNM, through Sony's libraries. The ways there, roughly by size:

- **A CPU Vulkan.** SwiftShader (or Mesa's lavapipe) is a Vulkan driver that runs on the CPU, with
  its shaders compiled by a JIT. Eden's side is done (patch 0013, tested with lavapipe above);
  SwiftShader with its Subzero JIT (no LLVM) is the one to port, as it's built to be portable. It's
  cloned in this build environment, but building it wasn't allowed here (newly downloaded code).
  Built for the PS4 (its JIT needs the system pool's executable memory, as dynarmic does), it would
  run Eden's Vulkan renderer unchanged; its frames would reach the screen as the null renderer's do
  now. Slow, but enough for 2D homebrew at 720p or below, and the shortest way to a first GPU-drawn
  picture.
- **A GNM backend for Eden's renderer.** Eden's Vulkan renderer rewritten against the PS4's GPU
  library, and its shader recompiler (SPIR-V now) taught to emit PS4 shaders. Fast, and a much
  larger project: the PS4's shader format isn't documented.
- Either needs the console's keys and firmware for commercial games, which Eden takes from the
  user's own Switch.

## The PS4 app

`out/EdenPS4.pkg` (title ID EDNP00001, version in `VERSION`). It opens on a HOME menu laid out as
the Switch's (`src/ui/`, since 03.20):

- **The row**: your software as tiles, the most recently played first (the first 12), the selected
  one framed (a pulsing cyan border) with its name above it, and **All Software** at the end (a grid
  of everything, with names). X starts a title; OPTIONS shows what it is and where (size, folder or
  USB drive, path). A homebrew program's name and icon come from the `.nro` itself; a game's from
  what its first boot learned (reading them takes the keys), kept in `/data/eden-ps4/cache/titles`;
  until then its tile is a cover made from its file's name. The package's programs have their own
  art.
- **The round buttons** below: **News** (what's new, how to report problems), **Games and Keys**
  (whether `prod.keys`, `title.keys` and the firmware are where they should be, the games found,
  where things go, and Import from USB), **Controllers** (the PS4 buttons each Switch button is, and Test Input
  Devices: the controller test), **System Settings** (Themes: Basic Black or Basic White; Graphics:
  Vulkan on the PS4's GPU or the software renderer, and which one is in use, and the resolution
  games render at, 100%, 75% or 50% of the console mode's; Console: handheld, 1280x720, the
  default, or docked, 1920x1080; About) and **Power** (closes the app). A lower resolution takes
  less of the PS4's GPU memory (480 MiB once the Switch's 4 GiB are mapped; a quarter at 50%) and
  less GPU work, but doesn't speed up a game held back by the CPU, which most are (stage 1 measured
  a PS4 core at 15-25% of a busy Switch core's speed).

  Since 03.30, also for speed (each item says what it does under it):
  - **Graphics, Shader Cache** (on): a game's shaders are kept in
    `/data/eden-ps4/user/cache/shader/<title id>`, with RADV's compiled pipelines, and built while
    it loads the next time, so they don't stutter when they first appear again. (Eden's frontends
    call `LoadDiskResources` themselves; this launcher didn't, so the cache was never used.
    Homebrew without a title id in its NACP has none.) On the desktop with Vulkan, a second run's
    first second went from 36 to 55 frames.
  - **Graphics, Asynchronous Shaders** (on): new shaders are built in the background instead of
    stopping the game while the PS4's CPU compiles them; what needs one is missing for a moment.
  - **CPU, Accuracy**: Auto, or Unsafe (stage 1 measured about 10% on vector maths, nothing
    elsewhere; some games misbehave with it).
  - **CPU, CPU Cores**: 7 (the default) or 6. The package now asks for the PS4's 7-CPU mode
    (`param.sfo` ATTRIBUTE 0x10002, as Crema's package does), which lets an app use the 7th core
    the system shares; with 6, the runtime keeps every thread started from then on to cores 0-5
    (`ps4_set_app_cores`).

  - **Graphics, GPU Accuracy** (03.50): Normal (the default) or High. High is Eden's desktop
    default: GPU-written memory read back whenever the game's CPU code might look at it (reactive
    flushing), safe reads of command lists, precise queries. Normal is what Eden's phone builds
    default to (Low, no reactive flushing): a few games need High for some effects.

  Not an option: tuning the recompiler for the Jaguar, where stage 1 found it slowest (calls x10,
  vector floats x12.5), is work on dynarmic itself.

  Speed in 03.50, everywhere (on the PS4, where the CPU is what holds a game back):
  - **Reads of memory the GPU caches** (patch 0052). Eden marks such pages so that a write
    invalidates the GPU's copy. Without fastmem the recompiler treated a marked page as unmapped for
    reads too, so each read went through a callback that asks the GPU for a flush area and can wait
    for the GPU: reactive flushing, always, on the CPU's hot path. Reads now stay on the fast path
    unless GPU Accuracy is High, as with fastmem.
  - **GPU macros compiled** (patch 0051, above), not interpreted, on the GPU thread.
  - **Thread priorities** (patch 0053, `port/ps4_runtime.cpp`). Eden's had no effect on the PS4,
    so the emulated CPU cores and the GPU thread shared their time equally with logging, loading and
    shader compiling. Now, as Crema does: those and the Vulkan worker at 650 (the default is 700),
    core timing and the vsync conductor just above (640, they wake for moments), shader compilers
    just below (720), the controller's polling and the hang watchdog at 600, audio at 400.
  - **Code caches** (patch 0055): 40 MiB for each of the three cores a game runs on, 8 MiB for the
    system's (whose services are HLE), instead of 32 MiB each: fewer flushes of a full cache, each
    a stutter while the code compiles again, in the same 128 MiB of memory that runs code.
  - **Three pipeline workers** (patch 0054), not one per CPU but one: each ACO compile holds memory
    in the app's heap until it's done.
- The user's icon and the time at the top; the controller and the buttons' hints at the bottom.
- A game without the keys: a dialog says where they go, instead of a loader error. After a run that
  didn't finish: its report first (the end of `log.txt`, `crash.log` and its last steps), to
  photograph.

Its settings are in `/data/eden-ps4/menu.txt`, the order played in `recent.txt`. The text is Inter
(`ui-fonts/`, SIL Open Font License); everything else is drawn by `src/ui/canvas.cpp` (anti-aliased
shapes from their signed distances, at 1920x1080 in the Switch's 1280x720 units). `ui-preview.sh`
draws each screen to a PNG on the desktop, with sample titles.

- **Breakout** (`breakout.nro`, in the package). Left stick or D-pad to move, X to launch the ball,
  OPTIONS to quit.
- **Blocks** (`blocks.nro`, in the package). D-pad or stick to move and to drop faster, Cross and
  Circle to rotate, up or Triangle to drop, the touchpad's click for the music, OPTIONS to quit.
- **GPU test** (`gputest.nro`, in the package): drawn with the Switch's GPU (stage 4, above).
  OPTIONS quits.
- **Controller test** (`padtest.nro`, in the package). OPTIONS and the touchpad's click together
  quit.
- **The test program** (`hello.nro`, in the package): stage 2's checks, text only.
- **Every `.nro`, `.nso`, `.nsp` and `.xci` in `/data/eden-ps4` and at the top of a USB drive.**
  A homebrew program that draws with the CPU (libnx's framebuffer or console, as the games and the
  controller test) works with either renderer; what draws with the GPU (games, deko3d, OpenGL)
  needs the Vulkan renderer on the PS4's GPU, or is drawn (slowly) by the software renderer.

When the program ends, the HOME menu comes back. Both sticks pressed in (L3 + R3) for a second stop
a program, for one without a way to quit or one that hangs; where its threads were goes to
`log.txt`. Without a controller, the test program boots after a countdown. If the last run didn't
finish, the menu first shows what that run left behind: the end of its output (`log.txt`), its
crash report (`crash.log`) and the last steps of its boot trace, until X is pressed. Stack addresses
are ELF addresses: symbolize them with `llvm-symbolizer --obj=<that build's ELF> 0x...`.
`build.sh ps4` keeps every build's ELF in `~/eden-ps4-elves`.

Until the program draws, its output and Eden's warnings and errors are on screen; they always go to
`/data/eden-ps4/log.txt`. Eden's data folders (NAND, SD card, keys) are in `/data/eden-ps4/user`.
Nothing is written outside `/data/eden-ps4`.

## Testing on a console

Nothing in stage 2 or 3 has run on a console yet. What to try, and what tells the most:

1. Install `out/EdenPS4.pkg` and start it. The HOME menu should show five programs. If the app
   closes or freezes before that, `/data/eden-ps4/boot-trace.log` has its last steps and
   `crash.log` its crash report (next start, the start page shows both: a photo is enough).
2. **The test program** (stage 2's checks): every line should end with "passed" or a result, and
   the start page come back.
3. **Breakout** and **Blocks**: the picture, the controls, the beeps (and Blocks' tune). Both
   show their frame rate. `boot-trace.log` gets a line every 10 s with the frames a second, how
   long each waited for a buffer off the screen (the display's pace) and how long scaling it took
   (the launcher's cost).
4. **Controller test**: each PS4 button should light its Switch button.
5. **GPU test**: the bands, the squares and GPU (clears), three circling sprites (2D blits of a
   DMA copy), a turning RGB triangle (shaders), a turning square with the sprite on it and a
   scrolling backdrop of diamond tiles over the bands (textures), a red and a green bar at the top
   right, each in front on its half (depth), a cyan square top left (a storage buffer), an orange
   triangle on the left (stencil), a yellow rectangle of lines with a white point in it, bottom
   right, a floor in perspective at the bottom (a grey checkerboard near, then red, orange, green,
   cyan and blue bands into the distance: mipmaps; darker stripes across it: a shader's branch),
   under the bars a red square left and a green one right (each fragment program's own depth),
   and a dark square of diamonds below them (an sRGB texture), a square of colour blocks at the
   top (a compute program's) and two of diagonal colour bands below it (a storage image, and
   the same as a multisampled texture),
   all drawn by the GPU emulation in software, and its frame rate as a green bar (6 pixels a frame
   a second). Its `log.txt` gets a
   line every 2 seconds, "Software draw: a second: ... ms rasterizing (on up to N threads)": what
   the console's cores make of the software GPU. A turns on its stress test (400 more sprites, each a
   draw; up and down change how many): the frame rate it keeps says how many sprites a 2D game could
   draw. B turns on the 3D stress test (12 large lit, textured, depth-tested quads): its frame rate
   says what a simple 3D scene would get.
6. Any CPU-drawn homebrew `.nro` copied to `/data/eden-ps4` (FTP) appears in the list. L3 + R3
   held for a second stops it.

Settings: each line of `/data/eden-ps4/settings.txt` (copied by FTP) that reads `NAME=value` is
set as an environment variable before Eden starts. `EDEN_SW_THREADS=5`, say, rasterizes on 5
threads instead of 4 (the PS4 build's default; an app has 6 cores), to see which the console likes;
`EDEN_SW_JIT=0` interprets shaders instead of compiling them (`log.txt` says when they're compiled).
`EDEN_PS4_HANG_SECONDS=60` makes the hang watchdog (below) wait 60 s for a frame instead of 20.
`EDEN_PS4_HEAP_TEST=1500` is a test of the app heap's growth: at start, with 4 GiB of direct memory
taken as the Switch's RAM will be, it allocates 1500 MiB and logs where each overflow heap came
from in `boot-trace.log`.

After a run, `/data/eden-ps4/log.txt` has the program's output and Eden's warnings and errors, and
`boot-trace.log` the launcher's steps (the audio port it opened, the first frame, the frame rate).
If a program's frames stop for 20 s after they had started, or none comes 90 s after it booted (a
game frozen, or stuck loading), the hang watchdog writes every thread's stack, each thread by name,
to `hang.log` (once, until frames come again): send it with `log.txt` when a game freezes.
Since 03.60 the logs name the code they point at themselves (the package's `symbols.zst`); the
build's ELF (kept next to the package in `out/` when it's sent, and in `~/eden-ps4-elves`) is still
what `llvm-symbolizer` takes for line numbers.

The easiest way to move files: open the address shown at the top of the HOME menu
(`http://<the PS4's address>:8080`) in a browser on a PC or phone on the same network. It takes
keys, firmware and games, and **Download all logs** gives every log above in one file, code
addresses named, to send instead of photos. **Profile summary** shows where a game's time went
when System Settings, CPU, Profiler is on.

## Testing in shadPS4

With 02.00: the start page lists the package's programs and an `.nro` put in `/data/eden-ps4`.
Breakout: X (shadPS4's keyboard: N), the game's first frame 1.6 s later, an AudioOut port opened for
its sound; X launched the ball, the paddle followed the stick (D), OPTIONS (Enter) quit to the start
page. The controller test showed Cross (N) as A and the stick (D) at full right, and quit with
OPTIONS and the touchpad (Enter and Space). Blocks played (the arrows, N, Up), and its best score
was written to the SD card by one run and read by the next. L3 + R3 (X and M) held stopped a game
and brought the list back, three times in a row in one session. A keyboard tap of a few milliseconds
can fall between two of a game's reads of the controller; a held key always counts. shadPS4 has no
sound here (no audio device), so the sound is tested on the desktop: there the WAV file has
Breakout's beeps, each at its pitch and length (the ball's launch 660 Hz for 60 ms, a brick about
1 kHz, a wall 330 Hz, a lost ball 160 Hz for 250 ms), in real time.

`shadps4-play.sh` runs the package's staging folder in shadPS4, headless, and plays a script of
key presses, waits on the boot trace and screenshots, read from stdin (its header lists them);
those runs above are such scripts. As for stage 1 (see
[its README](../switch-bench/README.md#testing-in-shadps4) for building shadPS4 and running it
headless): the PKG's staging folder `~/eden-ps4-build/stage` without
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
| null renderer: frames for frontends without a GPU renderer | stage 3's picture, above |
| audio: a frontend sink | stage 3's sound, above |
| null renderer: one layer at its own size | a frame with one layer (the usual case) isn't scaled to 1280x720 on its way to the frontend, which scales it anyway |
| host memory: clear large regions on Linux by dropping their pages | a homebrew program's 3.2 GiB heap, cleared in no time on the desktop (above) |
| audio: frontend sink streams play silently when the output can't open | a stream without an output still consumes its buffers at an output's pace, so a program waiting for them doesn't stall |
| vulkan: headless rendering for frontends that take frames | stage 4's Vulkan renderer without a window, above |
| null renderer: colour clears, DMA copies and 2D blits in software | stage 4's GPU test on the PS4, above |
| sw_blitter: nearest-neighbour scaling reads the right source pixel | an upstream bug the GPU test's sprites found, above |
| null renderer: shader draws in software | stage 4's software renderer, above |
| null renderer: textures in software draws | above |
| null renderer: faster software draws, on several threads | above |
| null renderer: depth testing, depth and stencil clears | above |
| null renderer: storage buffers, global memory and subgroup ops in software shaders | above |
| null renderer: points, lines, quad strips and polygons in software draws | above |
| null renderer: stencil testing and several render targets in software draws | above |
| null renderer: tick the rasterizer each frame, so CPU-written textures are seen | above |
| null renderer: report gathered writes before draws and clears | above |
| null renderer: indirect draws, inline index buffers and DrawTexture in software | above |
| null renderer: software draws find their programs without hashing the code at every draw | above |
| null renderer: small software draws queued and rasterized together on the rasterizing threads | above |
| null renderer: software fragment shaders run in 2x2 quads, branching per lane | above |
| null renderer: software draws recheck shaders by their code, and flush for reads only where they draw | above |
| null renderer: RGBA8 writes and bilinear filtering in arrays, sRGB in the fast path, fragment shaders' own depth | above |
| null renderer: compute shaders in software | above |
| null renderer: primitive restart, the provoking vertex, polygon offset, the alpha test and point coordinates in software draws | above |
| null renderer: multisampled render targets drawn at their samples' resolution | above |
| null renderer: software shaders read a uniform once for every lane | a constant buffer read at a constant offset, read once, not 16 times |
| null renderer: the depth test 16 pixels at a time, and only the varying components a shader reads | above |
| null renderer: software shaders' identities are their arguments, and moves copy what a value takes | above |
| null renderer: varyings interpolated in plane form, constant-index extracts a word's copy | above |
| null renderer: EDEN_SW_DUMP logs each software program's ops | above |
| null renderer: storage images and texel buffers in software shaders | above |
| null renderer: software shaders compiled to native code | above |
| null renderer: multisampled texel fetches, varyings after the depth test, a native code self-test | above |
| null renderer: software vertex shaders 16 vertices at a time, each distinct vertex once | above |
| vulkan: the PS4's GPU through Mesa's RADV, after Eden4 | stage 5, above |
| core: free the application's process before the kernel when loading fails | an upstream use-after-free on a failed boot: harmless with glibc, a crash on the PS4 (stage 5, above) |
| common: the PS4's thread names go to the app's runtime | named threads in `hang.log`, and heap arenas for the busy ones (above) |
| common: the GPU's page table takes PS4 memory as it is used | below |

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
- **The app heap's overflow**: past its 384 MiB, the heap grows in 32 MiB steps (larger for a
  larger request), first from regular flexible memory (about 390 MiB that nothing else in the app
  uses: Crema keeps it for Piglet), then from direct memory, but only while 256 MiB of it stay free
  for the GPU (32 MiB with the software renderer). Crema's one overflow heap of up to 512 MiB of
  direct memory would have left RADV about 100 MiB. Eden4 learned this on a console: one large
  overflow heap left its Vulkan driver 320 KiB in Clubhouse Games. Eden's CPU core threads and its
  GPU thread get heap arenas of their own (10 MiB each), as Crema's busy threads do.
- **The GPU's page table** (`common/multi_level_page_table.inc`): a game's GPU address space
  (37 bits) took 128 MiB of system flexible memory the moment it was made, mapped in full; it
  only reserves its range now, and takes 64 KiB at a time as the game maps GPU memory there, as
  on Windows (Eden4 found this too).
- **No fastmem**: the PS4 can't map the same memory twice for an app.
- Fiber stacks were already 512 KiB on the PS4 upstream (2 MiB elsewhere). Eden makes about 75
  fibers (one per emulated thread, most of them for HLE services).

Other PS4 choices, in the launcher and `ps4-eden.cmake`: Boost.Asio is built without kqueue (the
PS4 has no `sys/event.h`), thread priorities aren't set and thread names go to the runtime (for its
hang reports and heap arenas) rather than the system (not calls Crema makes on these consoles), and
FFmpeg is built without its Linux-only calls (below).

## Testing on the desktop

`test.sh` runs each program in the desktop build with scripted controller input and checks what it
printed, drew and played: the test program's checks, Breakout's frames and beeps, Blocks' frames,
tune and best score (saved by one run, read by the next), the controller test, stopping a program,
the GPU test (its fences, its frames having the picture, its shader triangle), and that a program
with libnx's full-memory heap starts at once. 21 checks, about 30 s.

## Building

Needs:

- Eden at commit `eb88a40` with `patches/` applied (`git am patches/*.patch`)
- CMake 3.31 or newer (`CMAKE=/path/to/cmake` if it isn't the one on `PATH`)
- Crema's PS4 toolchain and sysroot (`docs/BUILDING.md`; `toolchain/build-sdk.sh` builds its
  section 2) for the PS4 build
- clang 21 and lld for the test program and the game, and for the game libnx built with clang
  (`nx/build.sh`)

```
eden-ps4/fetch-deps.sh                # Eden's dependencies, as git clones, into ~/eden-deps
eden-ps4/build-ffmpeg.sh              # FFmpeg for the PS4 (the PS4 build only)
EDEN_DIR=~/eden eden-ps4/build.sh host    # desktop: ~/eden-host/bin/eden-ps4 <program.nro>
EDEN_DIR=~/eden eden-ps4/build.sh ps4     # out/EdenPS4.pkg
eden-ps4/hello/build.sh               # hello.nro on its own
eden-ps4/nx/build.sh                  # libnx and its toolchain, with clang, into ~/nx/devkitpro
eden-ps4/game/build.sh                # the games and the controller test (build.sh ps4 does too)
```

`nx/build.sh` builds what devkitPro's packages would give (their servers are blocked in some build
environments; GitHub's git isn't): devkitPro's newlib, libnx, compiler-rt's builtins (libgcc's
part), and elf2nro and nacptool, at pinned commits. clang stands in for devkitA64's gcc, under its
names. libnx needs a few changes for clang (`nx/libnx-clang.patch`): the thread pointer is
`tpidr_el0` written directly (clang has no `-mtp=soft`), the C11 atomics on plain variables go
through `__atomic` builtins (`clang_atomics.h`, force-included), three structures that end in
variable-length arrays (a GCC extension) are a fixed array or left out (the GPU channel submits,
which a CPU-drawn program doesn't use), and a weak symbol isn't also made global. lld needs the
linker script to give TLS a program header (`switch_clang.ld`). Its steps are the ones that built
this machine's toolchain, by hand; the script as a whole hasn't been run from scratch.

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
`EDEN_PS4_TIMEOUT` how long a program may run (default 60 s). For a program that draws, plays sound
and reads the controller, without a screen, speakers or a controller:

- `EDEN_PS4_FRAMES=<dir>` saves its frames as PPM images: the first, one a second, and the last
- `EDEN_PS4_AUDIO=<file.wav>` records its sound
- `EDEN_PS4_INPUT="<seconds>:<buttons>,..."` presses player 1's buttons, e.g.
  `"3:A,3.2:,5:Right,6:"` (`Stop` for the buttons stops the program, as L3 + R3 on the PS4)
- `EDEN_PS4_LOG=<filter>` sets Eden's log filter, e.g. `"*:Info Service.VI:Debug"`
- `EDEN_PS4_BOOTS=<n>` boots it n times in one process, as the PS4's HOME menu runs one program
  after another, and prints the heap in use after each (what a run leaves behind)
- `EDEN_PS4_FASTMEM=0` turns fastmem off, so that guest memory goes through the recompiler's page
  table walk as on the PS4 (which can't map memory twice)
- `EDEN_PS4_GPU_HIGH=1` is System Settings' GPU Accuracy High (Normal by default)
- `EDEN_PS4_MACRO_JIT=0` interprets GPU macros instead of compiling them

`test.sh` runs every test program this way and checks what it printed, drew and played
(`TEST_KEEP=1` keeps each run's whole log; a failed check prints the end of it). With
`EDEN_PS4_BIN` it runs another build: an AddressSanitizer one found two of 03.50's fixes (a core
dump of a run under load, a third).
