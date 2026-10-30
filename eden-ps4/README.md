# Eden for PS4: stage 3, Switch games playable; stage 4 begun, the GPU

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
