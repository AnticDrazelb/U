# Overnight work (2026-11-09 to 10)

The owner can't test tonight and asked for everything feasible on every part of the emulator. An
hourly scheduled check-in resumes the session (it survives usage-limit resets); each one takes the
first unfinished item below, finishes it (code, tests, the regression set both normally and with
`XB_TEST_FS_AS_PS4=1`, the FreeBSD VM tests), commits, pushes, rebuilds the packages, and records
what was done and verified here. The scheduled task is removed in the morning (after 14:00 UTC).

Order: what Halo will meet next after the network adapter, then everything else by how likely a
retail game is to need it. Each item says how it is checked without the game.

## Items

1. **The second USB controller (0xfed08000, IRQ 9).** PCI reports both, as every console has, but
   only the first is emulated: the XDK's USB driver sets up both and would wait on the second.
   Check: the usb sample and the kernel test still pass; a test that resets the second controller.
2. **USB as the XDK's driver uses it:** audit `usb.cpp` against the OHCI 1.0a register set (every
   register, the root hub's port bits, the done queue, frame number, interrupts) and fix gaps.
3. **(checked; DSP part moved to 10) The APU and DirectSound start-up:** what DirectSound waits on when it starts (the front end,
   the GP and EP DSPs' reset and idle bits, the mailboxes), against xemu's APU registers; make
   every wait it can make finish.
4. **NV2A coverage against xemu's method list:** every PGRAPH method xemu handles that this doesn't
   (Halo's log already shows class 0x39 method 0x180), PFIFO/PTIMER/PCRTC registers D3D reads.
5. **Kernel exports audit:** every one of the 366 against Cxbx-Reloaded's semantics; the ones that
   are stubs or simplified get done properly, each with a kernel-test check.
6. **(checked) XNet with no cable:** what the network stack does after the adapter (DHCP timeouts, timers);
   nothing must block a game's start-up.
7. **Everything the logs can tell from a run with no frame:** a watchdog report that names the
   loop a thread spins in (the code around its eip, the device registers it reads).
8. **(done) HANDOFF.md** brought up to date with all of tonight.
9. **Morning summary** below, the final packages sent.
10. **The GP DSP (a DSP56300 interpreter)**, if time allows: xemu runs the GP 1000 cycles a frame
    even with its DSP option off, so DirectSound may wait on GP microcode at start-up. Halo's next
    log will show it (APU registers at the top of "device registers by reads").

## Done

(each item: what changed, how it was checked, the commit)

- **1 and 2, USB.** Both MCPX OHCI controllers now exist: USB 1 at 0xfed08000 (IRQ 9) with nothing
  plugged in, as PCI reports it. Registers per OHCI 1.0a: reset values on HCR (HcFmInterval,
  HcLSThreshold), HcHCCA's low byte read-only, HcFmRemaining counting down, HcRevision/HcFmNumber
  read-only, interrupt routing never to SMM, frame-number overflow (FNO). The controller's
  internal hub (USB 2.0 ch. 11, 3 ports) sits on root port 3 with the gamepad on its port 1, as
  the console's controller is built and as xemu and Cxbx-Reloaded attach theirs (Microsoft's USB
  stack was written for it). Checked: kernel test (USB 1 answers, HCR, HCCA alignment); nxdk's
  stack enumerates the hub (address 2) then the gamepad behind it (address 3) in the usb and
  sdl_gamecontroller samples; with --pad a the sdl_gamecontroller sample shows "A:1" for
  controller 1 (045e:0202); regression 12/12 both ways.

- **7, what a hang shows.** Every device register access is counted per register (reads, writes,
  the last value) and kept in a ring of the latest 64, lock-free (diag::DeviceAccess, from
  DeviceRead/DeviceWrite). The end-of-run report and the watchdog's hang.log list the 16 registers
  read most, named by device ("nic", "apu", "ohci0"...), and the latest accesses: a game waiting on
  hardware shows the register it polls and the value it keeps getting, in one log (the network
  adapter's hang took a round trip to find). Checked: the httpd sample's report lists the adapter's
  registers; regression 12/12 both ways.

- **3, the APU front end, checked against xemu's.** The PIO window (free count 0x80, every method
  xemu's VP takes), trapped-method interrupts, notifies and the voice processor match; what differs
  is that the GP and EP DSPs' microcode doesn't run (moved to item 10, pending Halo's log).

- **4 (first part), NV2A against xemu's PGRAPH.** Classes: xemu does 0x12, 0x44, 0x62, 0x9f and
  0x97 (0x39, which Halo's log noted, it ignores too); this does all but 0x44 (pattern colours,
  which xemu only stores). Kelvin: the occlusion reports were missing. CLEAR_REPORT_VALUE,
  SET_ZPASS_PIXEL_COUNT_ENABLE and GET_REPORT now count the pixels that pass every test and write
  the report (timestamp, count, 0) into the report DMA, as xemu does; while counting is on, draws
  go to the software renderer (exact counts). Found on the way and fixed: **depth at the far plane
  was written back as 0** (GPU write-back and the software encode alike: in float, 16777215 + 0.5
  rounds to 2^24, which the 24-bit depth's shift into the depth-stencil word made 0), so every
  cleared depth pixel became the nearest depth whenever the GPU's surfaces went back to memory.
  Checked: tests/report-test (new, in the regression set): 5000 pixels for a 100x50 quad, the
  depth test halving a second one, a cleared count 0, in software and on the GPU; the ff, rt, tex
  and blit picture checks pass in both renderers; regression 13/13 normally, PS4-like and GPU.

- **5, kernel exports.** 36 of the 366 had no implementation (an unknown export's stub returns 0).
  Now done as NT does them: kernel queues (KeInitializeQueue, KeInsertQueue, KeInsertHeadQueue,
  KeRemoveQueue with its timeout, KeRundownQueue), device queues (KeInitializeDeviceQueue, the
  insert/remove calls by order and by key, KeRemoveEntryDeviceQueue; layout as nxdk declares it:
  Cxbx-Reloaded's offset comments disagree with its own field order), share access
  (IoCheck/Set/RemoveShareAccess, NT's sharing-violation rules on the FILE_OBJECT's bits), objects a
  program makes and names (ObCreateObject, ObInsertObject, ObReferenceObjectByName, through the
  device name table), the port-buffer reads and writes, KiUnlockDispatcherDatabase,
  IoQueueThreadIrp, IoStartNextPacketByKey; AvSet/GetSavedDataAddress keep the address. Left out on
  purpose: the IRP builders and IofCallDriver (IoAllocateIrp, IoBuild*Request, ...), because the
  Xbox's IO_STACK_LOCATION isn't settled (Cxbx-Reloaded asserts 0x18 bytes without the fields NT
  has; nxdk doesn't declare it) and a wrong layout is worse than the logged stub; Cxbx-Reloaded
  leaves all of them unimplemented. Checked: 11 new kernel-test checks; regression 13/13 in all three
  modes.

- **4 (continued), the hardware comparison (nxdk_pgraph_tests against the console's captures).**
  Fetched the suite and the 5816 captures again; a full run now matches 2290 of 3110 images (73.6%;
  the earlier session's run matched 1340). Fixed from it so far: **SET_COLOR_MASK 0 wrote every
  channel** (0 was taken as "not set"; the register now starts with all channels on, as pbkit and
  Direct3D set it, and 0 writes no colour), in both renderers; and the suite's runs were timing-
  dependent: the tests free each vertex buffer as they allocate the next, so the CPU rewrote
  vertices our GPU thread hadn't read yet. XB_NV2A_SYNC=1 (which run.py sets) makes a PUT write wait
  for the push buffer to be run up to it, as a GPU that keeps up; games keep their own pace. With
  both, Stencil goes from 0/8 to 8/8, the same on repeated runs. Also: tests/pgraph's runner takes
  suite names as the menu shows them ("Stencil func").

- **4 (continued): signed texture channels.** SET_TEXTURE_FILTER's signed bits (A 28, R 29, G 30, B
  31), unimplemented here and in xemu, now read a channel's byte as two's complement over 127
  before filtering; an unsigned combiner input clamps the negatives to 0 (worked out from the
  console's captures of the suite's signed-component tests); a bump stage takes a signed input's
  blue and green as they are. Draws with signed channels go to software (the GPU path's textures
  are unsigned 8-bit). Texture_signed_component_tests: 1/19 to 17/19.
  **Bump mapping (Bump_map 1/41, Bump_env_lum 1/42) investigated, not fixed:** the formula matches
  xemu's; the test's bump texture is SDL RGBA8888 (0x007f4500 is G 0x7f, B 0x45), and our stage
  reads it about 3% dark (G 0.484, B 0.258 for 0.498, 0.271) at the test's extreme magnification
  (2 texels over ~165 pixels, tent filter, quincunx kernel): a filtering/addressing detail to
  chase next with XB_NV2A_SYNC and one test (tests/pgraph's config takes a single test).

- **4 (continued): the signed blend equations, investigated, not fixed.** All 30 failing
  Blend_tests use ADD_SIGNED (0xf006) or REVERSE_SUBTRACT_SIGNED (0xf005), mapped here (as in
  xemu) to add / reverse subtract. The console's captures show the result doesn't depend on the
  blend factors (every dfactor column alike, the source at full strength with sfactor 0); four
  candidate rules (with/without factors, biased by 0.5 or two's complement) each gave 75/105, none
  right: left as it was, to finish with the test's exact colours.

- **6, no cable.** nxdk's httpd sample (lwIP over its nvnet driver, which sets the adapter up as
  Microsoft's does: MDIO, the PHY's link bits, the engine's idle bit, IRQ mask/status) runs to its
  DHCP wait, gets no answer, gives up after its 10 s and prints 0.0.0.0 for its addresses: nothing
  in the adapter's model blocks a program's start-up (124 adapter accesses, no fault unhandled).
  Halo's XNet is Microsoft's own stack; its next log lists the adapter's first 300 accesses
  ("nic:" lines) and the registers it reads most, if it waits on anything else.

- **4 (continued): depth and lines as the console draws them** (68e13b0). The suite's depth tests
  print the depth they read back, so the console's captures give exact values: integer depth is
  truncated (0xff8691 where rounding gave 0xff8692); SET_CONTROL0's Z_PERSPECTIVE_ENABLE stores the
  perspective-correct w in place of z (0xf5 for w 245.5) and **pbkit turns it on at start-up**
  (0x00110001), so every pbkit program draws with it on the console; polygon offset (bias, factor
  times the steepest slope) and the clip range's clamp or cull per pixel. Lines are rectangles
  SET_LINE_WIDTH/8 pixels wide. The GPU path does the w-buffer and polygon offset in a second
  fragment shader, used only by draws that need it. W_buffering 163 -> 233/265 (GPU 142 -> 202),
  Line_width 24 -> 40/61. ClassiCube's world checked in both renderers.

- **The GPU path leaked memory** (68e13b0): its pipelines were never freed; the blend suite grew it
  by 85 MB a second until the kernel killed it (taking a quarter of the GPU run's suites with it).
  Measured with gdb on the large allocations, then the cache's size; now bounded (128, the least
  recently used destroyed after a submit), and memory levels off.

- **GPU path: drawing in memory when the program sees the GPU idle; texel edges** (d11bbdb). A
  PGRAPH_STATUS read with the push buffer drained writes the GPU's drawing back, so the CPU reading
  a surface next has it; nearest filtering takes the texel after an edge as software does.
  Material_color_source 0 -> 28/28, Lighting_range 0 -> 3/3, Lighting_accumulation 0 -> 9/10,
  Specular 3 -> 18/22, Specular_back 0 -> 11/17, Blend_surface 6 -> 22/32 on the GPU path.

- **4 (continued): lighting, fog, lines with antialiasing** (3aa0d6a, c48629a, 7046753, b0fde41).
  Lighting: with SET_SPECULAR_ENABLE off the lit specular still goes into the diffuse (SEPARATE_
  SPECULAR ignored); a vertex program's oD1/oB1 get the same specular-enable and alpha rules
  (Lighting_control 13 -> 27/32). Fog: EXP2 fogs infinite/NaN distances fully; oFog is the x of
  what's written whatever the mask; an unwritten oFog carries one of six round-robin values
  (Fog_coord_vec4 23 -> 41/41, Fog_carryover 1 -> 13/14, Fog_exceptional_value 56 -> 68/96). Lines:
  an end near infinity looped the rectangle code for good (a hung job in a full run): rows now
  clamped and solved; with a supersampled surface the rectangle is in the picture's pixels.
  Full software run: 2307 -> 2425 of 3110 (78.0%) before the fog and antialiasing fixes (about
  +45 more). Open: the shadow comparator's raw-value tests (the console compares in units these
  tests don't make plain), bump mapping, the signed blend equations.

- **4 (continued): w, borders, colour key and alpha kill, points, surface alpha** (eec94b1, 3bf6b76,
  a022514, a7fefd2, 8581dd7). Positions' w clamped away from 0 and infinity as the NV2A does, only
  negative w clipped (W_param 16 -> 41/110; ClassiCube checked in both renderers). Textures with their
  own border texels (Texture_border 1 -> 12/18). Alpha kill (Direct3D's ALPHAKILL, used for
  alpha-tested foliage) and the colour key were not done at all: now in both renderers (Color_key
  6 -> 18/18 both). Points: SET_POINT_SIZE unless point params, pixels by coverage (Point_size
  8 -> 17/21, Point_params 16 -> 21/25, Point_sprite 4/4). Surface formats' alpha as blending
  reads it (Blend_surface software 16 -> 24/32, GPU 22 -> 32/32). Tried and kept out: rounding
  vertex positions to the 1/16 grid instead of truncating (Blend_surface 24 -> 9).
- **05:20: the session's machine restarted** (the queued hourly check-ins arrived at once); the
  GPU path's full run and the FreeBSD VM were restarted; nothing committed was lost.

- **4 (continued): vertex arithmetic, surfaces, YUV, blits** (1e223dc, eb52fcb, 465fb02, a62c3ea,
  e769a54, 0d2809a). Vertex programs now compute as the NV2A does (products truncated, the adder's
  28-bit window, anything times 0 is 0, colour outputs clamped per vertex; Vertex_shader_rounding
  49 -> 59/62, Attrib_float 2 -> 8/12). Clears stay inside the surface clip (Surface_clip 36 -> 47/47
  both renderers); G8B8 surfaces. YUY2/UYVY textures (games' video frames) and Y16 (Texture_format
  46 -> 50/52, Texture_render_target 37 -> 41/42). Image blits clipped by a clip rectangle object
  (Image_blit 34 -> 40/41). Software nearest filtering at texel edges as the GPU path's (Blend_surface
  32/32 both renderers). FreeBSD VM tests pass (05:47); packages rebuilt 05:57.
  Tried and reverted: DOT_STR_3D with a cube map as a cube lookup (the console shows something
  else). Open: the combiner suite's background band (its drawing helper isn't in the suite's
  source checkout here).
