# Performance notes

What has been changed for speed on top of Crema v03.30, how to check it on a console, and the
next steps that need a console to try.

## In this fork

| Change | Where | Default | Checked how |
|---|---|---|---|
| Render Resolution 100 / 75 / 50% | Settings > Display | 100% | builds (host compiler); patch applies |
| Texture Quality Full / Reduced / Low | Settings > Display | Full | builds; patch applies |
| Speed Preset Off / Fast / Fastest | Settings > Emulation | Off | builds; patch applies |
| Core Layout Spread / Clustered / Split | Settings > Emulation | Spread | builds; patch applies |
| Direct Vertex Data Off / Static / All (Experimental) | Settings > Emulation | Off | builds; driver part tested on the host (`tests/host`); patch applies |
| SSE4.1 quad index unpacking (GPU thread) | `LatteIndices.cpp` | always | bit-exact vs. scalar over 30000 random cases, ASan/UBSan clean; ~8x (16-bit), ~3.7x (32-bit), ~4x (auto) on a desktop CPU at `-O3 -march=btver2` |

None of it has run on a PS4 yet.

## Testing on the console

1. Diagnostics: everything off except **Performance Log** (and the FPS counter).
2. Pick a scene that's easy to repeat (a race's start, a town) and note the FPS and the log's GPU
   and thread loads with every new setting at its default.
3. Change **one** setting, restart the game, same scene, note the FPS again:
   - Core Layout: Clustered, then Split (CPU-limited games: Paper Mario, Breath of the Wild).
   - Render Resolution 75% / Texture Quality Reduced (only matter when the GPU load is high).
4. With **Profiler** on, the log's `tsc:` line gives the round trip from core 0 to each core:
   cores 1-3 should be clearly faster than 4-6 (same module). If not, Core Layout's module split
   is wrong for this console and should stay on Spread.
5. Keep what helps, report what breaks (title, setting, what it looked like).

## Direct Vertex Data (Experimental)

**Settings > Emulation > Direct Vertex Data**, Off by default. The GPU reads the game's vertex
buffers straight from the Wii U's memory instead of from copies in Cemu's buffer cache: the PS4's
CPU and GPU share one memory, so the copying, lookups and change tracking the GPU thread does for
every draw (and the cache's 164 MiB of GPU memory, re-uploads after eviction) can go. Uniform
buffers, index data and textures are unchanged.

- **Static Data**: only vertex data the game hasn't flushed from its CPU cache for 4 frames. On the
  Wii U the CPU must flush what it writes before the GPU reads it, so a flush is a reliable "this
  changed" signal; everything recently changed still goes through the buffer cache.
- **All Data**: every vertex buffer in the Wii U's MEM2 (`0x10000000-0x50000000`).

Data a draw wrote with streamout lives only in the buffer cache, so it is never read directly
until the CPU writes there again. Buffers outside MEM2 (MEM1, other arenas) always use the cache.

How it fits together:

| Layer | File | What |
|---|---|---|
| Setting | `src/gui/ps4/launcher/CremaSettings.cpp` | `direct_vertex` = off / static / all |
| Memory | `src/util/MemMapper/MemMapperPS4.cpp` | `CremaSetGpuReadable`: MEM2 pages get GPU read access (`sceKernelMprotect`), checked with `sceKernelVirtualQuery` |
| Import | `src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.cpp` | `CremaImportGuestMemory`: MEM2 as one `VK_EXT_external_memory_host` buffer |
| Driver | `port/mesa/ac_ps4_drm.c` | user-memory buffers: GPU address = CPU address, nothing mapped, unmapped or released |
| Binding | `src/Cafe/HW/Latte/Core/LatteBufferData.cpp` | per vertex buffer: direct when `LatteBufferCache_CremaDirectUsable`, else the cache |
| Tracking | `src/Cafe/HW/Latte/Core/LatteBufferCache.cpp` | last CPU flush per 64 KiB block; streamout marks |

Any failure along the import (no driver support, the kernel refusing GPU access, the allocation)
logs `Direct Vertex Data: off (...)` and leaves the buffer cache doing everything. Success logs
`MemMapper: ... GPU-readable`, `radv/ps4: bo N: user memory at ...` (gpu/radv-ps4.log) and
`Direct Vertex Data: static data - the GPU reads vertex buffers in 10000000-50000000 ...`.

The known risk: Cemu tells a game its GPU work is done once the GPU thread has processed it,
before the PS4's GPU has drawn it. A game that rewrites a vertex buffer right then can have it
drawn half-changed for a frame (flicker, torn shapes) - the buffer cache's copy hides this. Static
Data keeps recently changed data on the cache for that reason. A wrong GPU address would be a GPU
page fault (CE-34878-0); the import refuses anything unaligned, inside the driver's own GPU
address window or beyond the GPU's 1 TiB range.

### Testing it on the console

1. Diagnostics off except **Performance Log** and the FPS counter. Graphics Accuracy **Accurate**.
2. Start a game with Direct Vertex Data **Off**, note the FPS and the GPU thread's load in a scene.
3. **Static Data**, restart the game: check log.txt for the `Direct Vertex Data:` line, then the
   same scene. Look for missing, stretched or flickering models.
4. **All Data**, same again.
5. Report: game, setting, FPS before/after, and anything that looked wrong (or the crash code).

`tests/host/run.sh` runs the host tests (no console): the driver's user-memory buffers through
RADV's exact call sequence, and the eligibility tracking, both under AddressSanitizer.
