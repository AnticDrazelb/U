# A draw's work on the GPU thread: what the other Switch emulators do (2026-11-07)

04.84-04.85 measured each draw on Eden's GPU thread in Breath of the Wild outdoors (about 39,000
draws a second, the thread 100% busy). This compares each measured part with how the other Switch
emulators that have their own GPU emulation do it:

- Ryujinx, read from Ryubing (`git.ryujinx.app/ryubing/ryujinx`, `a61a426`: `src/Ryujinx.Graphics.Gpu`);
- Strato, Skyline's continuation, written from scratch for phone CPUs (`github.com/strato-emu/strato`:
  `skyline/gpu`);
- Eden upstream (`git.eden-emu.dev`, `885b333`).

The Yuzu forks (Citron, Sudachi, Torzu) share our design. 04.12's
[ryujinx-comparison.md](ryujinx-comparison.md) covered uniform buffers and pipeline keys; this is
the whole list, with 04.85's numbers.

## The short answer

- **Eden upstream has nothing new here.** Since our base, its video code changed by rendering fixes
  and an Android vertex-buffer switch: no speed work.
- **Ryujinx and Strato share one idea that Eden lacks entirely: sequence numbers.**
  - A resource (a buffer, a texture pool, a texture binding) is checked against the game's memory
    at most once per "sequence", and every later draw that uses it skips the check.
  - Ryujinx advances the sequence at syncpoint increments, texture and sampler cache invalidations,
    and each frame shown: a few times a frame. Strato keeps one per channel and one per buffer.
  - Eden checks every bound buffer and texture on every draw. That's our "vertex/index", "uniforms",
    "descriptors" and "image views" parts: about 9 of the draw path's 17.3 us.
- **Both write the game's constant-buffer updates into the GPU buffer directly.**
  - Eden writes guest memory, queues the range, invalidates the range in the caches before the next
    draw (our "flush" part, 2.0 us), and uploads it at the next check.
  - Ryujinx stages consecutive updates, writes them once with a check for identical data, and marks
    the buffer dirty. Strato writes straight into the buffer's host copy, or through a GPU-side copy
    when the GPU may still be reading it.
- **Both re-bind only what changed.** Vertex and index buffers, viewports and the like are
  refreshed only when their registers were written; otherwise only the (sequence-gated) sync runs.
  Eden already re-binds by dirty flags, but syncs every buffer on every draw.

## Part by part (us a draw, 04.85, Breath of the Wild outdoors)

| Part | Us | Eden | Ryujinx | Strato |
|---|---|---|---|---|
| vertex/index | 3.3 | re-bound by dirty flags; **every vertex and index buffer synced (checked for the CPU's writes) every draw** | re-bound only when dirty; otherwise `SynchronizeBufferRange`, which is **once per sequence** per buffer | refreshed only when its registers were written; a buffer the CPU changes often copied once per change into a ring ("megabuffer") |
| of those two, buffer checks | 1.8 (3.5 a draw, 1 in 50 uploading) | each check walks the page bitmap and **re-protects any unprotected pages in the range**: the GPU thread's ~2,900 `mprotect` calls a second | small buffers: one tracking handle each; the check is the sequence number and a dirty flag; re-protected only when dirty | per-buffer sequence number advanced on each modification |
| uniforms | 3.2 | every uniform buffer up to 4 KiB copied into a stream buffer every draw (94% of binds point elsewhere each draw in BotW) | bound as ranges of cached buffers; updates written into them | small ones through the megabuffer |
| flush | 2.0 (about 0.4 of it hand-overs, from 04.84's cost per hand-over) | the memory manager's queued constant-buffer writes invalidated in the buffer and texture caches before every draw | no such step: updates go to the buffer, marked dirty | no such step |
| descriptors + image views | 1.4 + 1.5 | each bound texture's descriptor read from guest memory, compared, its view looked up in a hash map and `PrepareImageView`'d every draw | the texture pool checked once per sequence; a binding with the same handle and an unchanged pool **reuses its cached texture** | a cache per texture slot: **checked this sequence, returned as is**; otherwise the header compared |
| dynamic state | 1.5 | 12 update functions each checking their dirty flags | 28 register groups, only the dirty ones' updates run | state objects refreshed only when their registers were written |
| render targets | 1.0 | `UpdateRenderTargets` and a feedback-loop check every draw | by dirty group | by dirty state |
| pipeline | 0.8 | 04.15: the last pipeline reused when its key's registers didn't change (70% of lookups) | by dirty group | by dirty state |
| the rest (descriptor set, record, image descriptors, locks, tick) | 2.9 | - | - | - |

## What this means for us

Ranked by gain against risk. Gains are shares of the GPU thread's ~25.6 us a draw, so 10% is about
10% more draws a second, roughly 1 fps in Breath of the Wild outdoors.

1. **Skip buffer checks when nothing can have changed** (exact, low risk; up to ~7%).
   - Keep a modification count per memory tracker region (4 MiB), bumped whenever a page in it is
     marked CPU-modified or unprotected.
   - A buffer remembers the count from its last check; the same count means the check would find
     nothing and re-protect nothing, so it's skipped.
   - That's Ryujinx's dirty flag without its sequence rule: it changes no behaviour, only skips work
     whose result is known.
   - Removes most of the 1.8 us and the GPU thread's `mprotect` calls (another ~1.7%).
2. **Texture bindings cached per slot** (medium; up to ~5%).
   - Reuse a slot's view without reading its descriptor or looking it up, when neither the texture
     pool's memory nor the image changed since that slot was last resolved.
   - The same kind of count as item 1, kept for the pool's pages and per image.
3. **Constant-buffer updates written into the buffer** (larger; up to ~7%, and less uniform copying).
   - Write the game's inline updates into the buffer cache's buffer as Ryujinx and Strato do,
     instead of guest memory plus a per-draw invalidation.
   - First, 04.86 splits the "flush" timer into its two parts and times the invalidation's buffer
     and texture sides, to confirm where the 1.6 us goes.
4. **Sequence numbers proper** (Ryujinx's rule: checked once between syncpoints). This would go
   beyond items 1-2, but it changes behaviour: a CPU write that lands between syncpoints would be
   seen one sequence later. Only if items 1-2 leave the checks expensive.

Items 1 and 2 together are 04.86's candidates, with the timer split for item 3. Combined, items 1-3
are perhaps 15-20% of the GPU thread: Breath of the Wild outdoors from ~10 towards ~12 fps. That
helps every game the GPU thread holds back, not only Breath of the Wild.
