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

## Next: the GPU reading the Wii U's memory directly

Cemu has a path (`VulkanRenderer::m_useHostMemoryForCache`) where all of the Wii U's memory is
imported into Vulkan and draws bind vertex and uniform buffers straight from it: no buffer cache,
no per-draw change tracking, no copies. Upstream compiles it out (on a PC the graphics card would
read it over PCIe). On the PS4 there's one memory, so the reason doesn't apply, and the GPU
thread - the limit in Mario Kart 8 - would drop all of `LatteBufferCache_Sync`.

What it needs:
- `util/MemMapper/MemMapperPS4.cpp`: commit the Wii U's pages with GPU read access as well
  (`PS4_PROT_GPU_READ`, 0x10). They are already CPU-cached direct memory on the coherent (Onion) bus.
- `port/mesa/ac_ps4_drm.c`: a user-memory buffer (`VK_EXT_external_memory_host`) whose GPU address
  is the CPU address it was created from (GPU addresses are CPU addresses on the PS4, and direct
  memory can't be mapped twice), with RADV's VA allocator told to use it.
- `VulkanRenderer.h`: `m_useHostMemoryForCache` back to a run-time switch, behind a Diagnostics
  setting, off by default.

Risks: with Graphics Accuracy Fast the GPU may read data the game has already moved on to rewrite
(the buffer cache's copy hides this today); Onion bandwidth is lower than the GPU's own path; a
wrong address is a GPU page fault (CE-34878-0). Try it on one game with Graphics Accuracy Accurate
first.
