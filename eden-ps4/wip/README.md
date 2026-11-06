# Work in progress: 04.83, the GPU thread split (paused 2026-11-06)

Not in the build. `patches/` and the frontend are at 04.82 (shipped). The work so far is kept here
as two diffs, so the tree stays buildable. The design is in `docs/gpu-thread-split.md`.

- `0173-gpu-thread-split-eden.patch`: the Eden side. Apply it to `~/eden` after patches 0001-0172
  with `git -C ~/eden apply`. It adds `src/video_core/gpu_split.{h,cpp}` (GpuBackend,
  RasterizerProxy, the shadow Maxwell3D sync) and changes channel_state, channel_state_cache,
  dma_pusher, engine_upload, maxwell_3d, gpu.cpp, gpu_thread.cpp, and the GL/Vulkan state trackers.
- `0173-gpu-thread-split-frontend.patch`: the eden-ps4 side. Apply it with `git apply` in
  `eden-ps4/`. It adds the Graphics > GPU Threads (One/Two) menu item, `gpu_threads=` in the
  config, and `EDEN_PS4_GPU_SPLIT` set from it. It also adds "GPU threads" to the setup log line,
  the back end's busy %, tasks/s and drains/s to the 30 s line, VERSION 04.83, and the news entry.

The switch is `EDEN_PS4_GPU_SPLIT=1` (off by default). With it off, none of the split's objects
exist and the GPU thread runs as before.

## Where it stands (desktop build, `ninja -C ~/eden-host eden-ps4`)

- **Split off:** `test.sh` shows 0 failed.
- **Split on:** gputest runs at 60 fps. Its frames match the single-thread run's at the same frame
  numbers. The only difference is the fps bar (`gputest.c:1806`, `rect(10, H-16, fps*6, ...)`): 61
  against 60 is timing, not a rendering bug.
- **Bug 1, a hang:** in one full `EDEN_PS4_GPU_SPLIT=1 TEST_KEEP=1 ./test.sh` run, gputest stayed at
  0.1 fps from its first frame. The harness killed it, so "exits on +" and the frame checks failed.
  It didn't come back in 18 runs of gputest alone (on all cores, on 2, and on 1), so it depends on
  timing. No stacks were caught yet. The full run ended "7 failed", in three separate gputest runs (gputest, gputest_interp with the macro interpreter, gputest_vertices), so in the suite it comes back readily, alone it does not: exits on +; submits GPU work and waits on its fences; its frames have the picture; a texture the CPU rewrites is drawn as rewritten; and with the macro interpreter; exits on +; draws them.
- **Bug 2, a crash at exit:** pinned to one core (`taskset -c 0`), 1 run in 6 crashed with SIGSEGV
  right after "gputest: channel closed" ("Unhandled SIGSEGV" from dynarmic's handler, exit 139). A
  likely cause: `GPU::Impl::ReleaseChannel` is `UNIMPLEMENTED()`, so the channel's ChannelState is
  freed when nvhost_gpu closes. That frees the front Maxwell3D and the shadow, while the proxy's
  `channels` map and `bound` still point at them, and the back end may still have queued tasks that
  use them. Next step: get a core dump (`ulimit -c unlimited`, core_pattern is `core` in the cwd,
  run on one core until it crashes, then `gdb bin core`, `thread apply all bt`). Then make channel
  teardown wait for the back end. One way: a ChannelState destructor or nvhost_gpu close hook that
  calls `backend.Drain()` and removes the proxy's entry, clearing `bound`.

Scripts used (scratchpad, not kept): runs of gputest with
`EDEN_PS4_GPU_SPLIT=1 EDEN_PS4_DATA=... EDEN_PS4_FRAMES=... EDEN_PS4_INPUT="5:Plus,5.5:" EDEN_PS4_TIMEOUT=20`,
taking gdb stacks on a hang, and comparing frames byte for byte at the same frame numbers.

## To finish 04.83

1. Fix bugs 1 and 2. Then run `EDEN_PS4_GPU_SPLIT=1 ./test.sh` several times until it shows 0
   failed every time, plus `./test.sh` with the split off.
2. Export the Eden side as `patches/0173-*.patch` (commit in `~/eden`, then
   `git format-patch -1 --start-number 173 -o patches`), and apply the frontend diff.
3. Run `CMAKE=~/cmake331/bin/cmake ./build.sh ps4` twice. Copy `out/NewEden.pkg` to
   `out/NewEden-04.83.pkg`.
4. Update STATUS.md, README.md, and `docs/gpu-thread-split.md` (status: in testing). Commit and
   push to eden-ps4-stage2.
5. User test: Breath of the Wild outdoors with GPU Threads set to Two, then set to One (Profiler
   on, played last). Also Animal Crossing, Mario Kart 8, and Captain Toad. Look at the 30 s line's
   back end busy %, and the fps against 04.82.
