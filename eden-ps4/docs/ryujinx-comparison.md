# Ryujinx's GPU frontend and translation cache, next to Eden's (for the PS4)

Written for 04.12's choice, after 04.08-04.11 showed that a race's start (and the headroom above
30 fps) comes down to Eden's GPU thread: ~28-29 us of its time per draw on the PS4's Jaguar, and
800-1,250 draws a frame at a start. Eden is Yuzu's fork (its dynarmic, Maxwell emulation, Vulkan
backend, buffer and texture caches are Yuzu's design), so the comparison that shows another way of
doing things is Ryujinx, a separate design (C#, its own recompiler ARMeilleure, its own GPU
emulation). Read from Ryubing's continuation of Ryujinx (`git.ryujinx.app/ryubing/ryujinx`,
commit `a61a426`): `src/Ryujinx.Graphics.Gpu` (GPFifo, Threed, Memory), `src/ARMeilleure/
Translation/PTC`. Nothing here is code to port: Ryujinx targets desktop CPUs, and every idea is
measured on a console before it stays.

The GPU thread's time at a race start (04.09's samples, ~90-120 s, 04.08's build): the command
stream (the DMA pusher, the engines' methods, macros) ~10%, memcpy 3.3%, the texture cache's
`SynchronizeAliases` 2.7%, `GpuToCpuAddress` 1.9%, memory tracking (`WordManager` and the like)
~2.5%, `UpdateVertexBuffer` 1.5%, `FixedPipelineState::Refresh` 1.3%, image views and
descriptors ~2.5%, uniform buffer binding 0.6%; idle 26%.

## Candidates, most promising first

| # | What | Ryujinx | Eden | Likely gain (GPU thread) | Risk | First step |
|---|---|---|---|---|---|---|
| 1 | Uniform (constant) buffers | updates gathered and written once, identical data skipped; buffers bound as cached ranges, only changed ranges uploaded | each draw copies every uniform buffer up to 4 KiB into a stream buffer (unless 98% of the cached path's binds hit); identical updates invalidated like any other | the memcpy (3-5%) and the stream buffer and descriptor work around it, part of memory tracking: several % | medium: Eden doesn't read GPU writes back at Low accuracy, so "identical" needs the buffer cache's "not GPU-modified" too | counters (below) |
| 2 | Pipeline state by groups | 28 groups; a register write marks its group dirty through a register-to-group table; a draw runs only the dirty groups' updates | `FixedPipelineState::Refresh` rebuilds the whole key every draw, then hashes and looks it up | 1-2% | medium: a register missed in the table leaves a stale pipeline (wrong rendering); a check mode recomputes every Nth draw and counts mismatches | after 1 |
| 3 | GPU fences and syncpoints | (to compare if 04.11 shows the main thread's waits are on nvdrv's `NVCTRL::NvEvent_*`) | host syncpoints signalled by the GPU thread and the Vulkan fences | ? | ? | 04.11's log |
| 4 | Command processing | per-word loop, fast paths for inline-to-memory and constant buffer data | per-word loop, multi-method batching (CallMultiMethod), runs of register writes in one loop (04.08), the 3D engine's straight into its registers (04.10) | little left | - | - |
| 5 | Persistent translation cache (PPTC) | the recompiler records a relocation for every absolute address it emits (page table, dispatch stub, function and delegate tables); the cache keeps code and relocations, patched on load; thrown away when the recompiler's version changes; only functions that ran enough | none; dynarmic embeds absolute addresses with no record of them | boot and loading times; a race's start translates only a few % of a core's time | high (stale code: crashes hard to trace) | later (the tester's order: third) |
| 6 | Memory (host-mapped) | guest memory mapped for direct access | fastmem (03.84 on) | nothing new for Mario Kart | - | for Bluey or Silksong, with their logs |

## 1. Uniform buffers

**Ryujinx.** `GPFifoProcessor.TryFastUniformBufferUpdate` takes a whole non-incrementing run of
constant buffer data at once. `ConstantBufferUpdater.Update` appends it to a staging array while
the writes follow each other in memory; `FlushUboDirty` (when they stop following, before a draw,
at the end of a command list) writes the lot with `WriteWithRedundancyCheck` and, only if it
changed anything, tells the buffer cache once (`ForceDirty`). At a draw, `BufferManager.
CommitGraphicsBindings` binds uniform buffers as ranges of cached buffers when their bindings
changed, and otherwise only synchronizes them (`UpdateBuffers`: the ranges modified since).
No per-draw copy.

**Eden.** `Maxwell3D::ProcessCBMultiData` writes the data into guest memory
(`MemoryManager::WriteBlockCached`) and adds the range to an invalidation accumulator, which
`FlushCaching` invalidates at the next draw (the buffer cache then marks it CPU-modified).
`BufferCache::BindHostGraphicsUniformBuffer` has two paths, chosen each frame (`TickFrame`): if
fewer than 98% of the cached path's binds found the data unchanged (`uniform_cache_hits` against
`uniform_cache_shots`), every uniform buffer up to 4 KiB (`DEFAULT_SKIP_CACHE_SIZE`) is copied from
guest memory into a stream buffer on every draw (`ReadBlockUnsafe` into `BindMappedUniformBuffer`);
otherwise the cached path (`SynchronizeBuffer`, upload of what changed, bind if needed).

**Why it matters on the PS4.** At 800-1,250 draws a frame, a copy of each bound uniform buffer on
every draw is a lot of memcpy, stream buffer space and descriptor work on a 1.6 GHz core. And a
game that rewrites a constant buffer with the same values every frame looks, to Eden, like a
change: it lowers the cached path's hit rate, which keeps Eden on the copy-every-draw path.

**What to measure first** (cheap counters, the 30-second line): the constant buffer update
bytes a second and how many were identical to what memory held; the uniform buffer binds a
second by path (copied, cached), the bytes copied; the frames in each mode.

**The change, if the counters show identical rewrites:** in `ProcessCBMultiData`, skip the write
and the invalidation when the data equals guest memory and the buffer cache says that range isn't
GPU-modified (Eden at Low accuracy doesn't read GPU writes back to guest memory, so guest memory
can be stale there). Test: a check mode that compares (counted, not acted on) on a first console
run; the harness can't run Eden's GPU code.

**A larger change, if the copies dominate:** keep the cached path for uniform buffers (or raise
the 98% threshold's tolerance) once identical rewrites no longer count as changes. Measured with
the same counters, before and after.

## 2. Pipeline state by groups

**Ryujinx.** `StateUpdateTracker<TState>` builds a table from each register (by its offset in
the 3D engine's state struct) to one of 28 groups (`StateUpdater`'s entries: vertex buffers,
attributes, blend, depth, stencil, viewports, rasterizer...). A register write calls `SetDirty`
(one table lookup, one bit); at a draw, `Update` runs the dirty groups' callbacks only, each
updating its part of the pipeline state and the backend's.

**Eden.** Maxwell3D's dirty flags (`dirty.tables`, filled in `dirty_flags.cpp` and the Vulkan
state tracker) already map registers to flags, the same mechanism; but `PipelineCache::
CurrentGraphicsPipeline` calls `FixedPipelineState::Refresh` on every draw, which reads every
register in the key (and the draw's topology, the engine state), then hashes the key and looks it
up.

**The change:** a dirty flag (or a few) for the key's registers, filled from `Refresh`'s reads;
`Refresh` (and the hash) only when one is set or the topology changed. Test: a check mode that
also refreshes every Nth draw and counts keys that differ (a register missing from the table); on
a console for a session before it's trusted.

## 3. Fences and syncpoints

Pending 04.11: if the main thread's waits at a race's start end on nvdrv's syncpoint events, how
Ryujinx signals host syncpoints from GPU completion (its `SynchronizationManager`) is next to read
against Eden's.

## 5. Persistent translation cache

Ryujinx's PPTC is what a persistent JIT cache for Eden would need: relocations recorded at
emission for each absolute address, a version that throws the cache away when the recompiler
changes, and only hot functions kept (its profiler). For dynarmic that means recording every
embedded address (the fallbacks, the dispatcher, the fast dispatch table, block links, the return
stack's code pointers, the callbacks' objects), a cache rebuilt after each Eden for PS4 release,
and each block checked against the game's code (and the code its flags look-ahead read). The
tester's logs put translation at a few % of a core at a race's start: the gain would be boot and
loading, so it stays third in the tester's order.
