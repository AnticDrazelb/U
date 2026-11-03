# The first race after a boot, and what Eden remembers between runs

Investigation of the tester's report (Mario Kart 8 Deluxe, 04.15/04.16): the first race after
launching runs its countdown at ~24-26 fps and stays near 24 for a few seconds before recovering
to 28-34; every later race in the session, other tracks included, is smoother; after a full exit
and relaunch the first race stutters again. Question: is it shader translation / Vulkan pipeline
creation, and can it be cached on disk and prepared during the 5-10 s black screen?

Sources: the code (`video_core/renderer_vulkan/vk_pipeline_cache.cpp`, `video_core/shader_cache`,
the PS4 frontend's boot in `eden-ps4/src/main.cpp`, Mesa/RADV as built for the PS4), and the
tester's logs `cb53c1aa` (04.15, Mario Kart session: a Grand Prix, first race ~150-190 s, a
second track loaded ~516 s).

## 1. What Eden already has

**Eden's shader cache ("transferable"), per game:** `<shader dir>/<title id>/vulkan.bin`.
Every time Eden creates a pipeline it has not seen (`CurrentGraphicsPipelineSlowPath`,
`CreateComputePipeline`), it appends the pipeline's key and the guest shaders it was made from
(the Maxwell code and the state the translation needs) to that file, on a background thread
(`SerializePipeline`). It holds *inputs*, not results: no SPIR-V, no GPU code.

At boot, before the game runs, the PS4 frontend calls `LoadDiskResources` (main.cpp, "loading
the shader cache of ..." in the log). It reads every entry and, on worker threads, translates each
shader again (Maxwell -> IR -> SPIR-V) and creates each pipeline; it waits for all of them. Then
the game starts. In the log: "loading the shader cache" at 18.9 s, the game's first activity at
28.2 s: **that is the ~9 s black screen**.

**The Vulkan driver's pipeline cache, per game:** `<shader dir>/<title id>/vulkan_pipelines.bin`
(setting `use_vulkan_driver_pipeline_cache`, on by default, not changed by the PS4 frontend). A
`VkPipelineCache` created from that file at boot (`LoadVulkanPipelineCache`) and passed to every
pipeline creation; written back (`SerializeVulkanPipelineCache`) after the boot's pipelines are
built, every 128 new pipelines (at most every 30 s, on a background thread), and when the
pipeline cache is destroyed (leaving the game).

**RADV on the PS4:** Mesa is built with `shader-cache=disabled` (no Mesa disk cache) and
`radv-build-id=60e95b78...` (a fixed build id, so the pipeline cache UUID is the same every boot
and RADV accepts the file). A pipeline found in the `VkPipelineCache` skips RADV's compiler (NIR,
ACO); Eden's own translation to SPIR-V is still done at boot for every pipeline, since the key
the driver looks up is the SPIR-V's hash.

## 2. What survives a full exit and relaunch

| What | Kept on disk | Used at the next boot |
|---|---|---|
| Pipelines seen (keys + guest shaders), `vulkan.bin` | yes, appended as created | all rebuilt during the black screen |
| RADV's compiled GPU code, `vulkan_pipelines.bin` | yes, at boot, every 128 new, at exit | given to every pipeline creation |
| Eden's SPIR-V | no | translated again at every boot |
| The CPU recompiler's translated code (dynarmic) | **no** | **translated again as the game runs** |
| Textures and buffers (texture cache) | no (game data, uploaded from the game's memory) | made again when the game uses them |

## 3. What is created during gameplay (Mario Kart, 04.15 logs)

10-second lines (`new: N pipelines (M draws skipped), images, buffers`) and the 30-second line's
recompiler figures:

| | First race's start (150-190 s) | Second track's race start (~520-540 s) |
|---|---|---|
| New pipelines (not in the shader cache) | 1, 0, 0, 2 | 180 while the new track loaded, then 10, 0 |
| Draws skipped waiting for a pipeline | 0 | 0 |
| New images | 133, 68, 28, 141 | 2,708 loading, then 510, 517 |
| CPU recompiler | 1,183 then 891 blocks/s: **173 then 134 ms of the emulated cores' time each second** | 146 blocks/s, 22 ms/s |
| Draws a frame | ~800 | ~790 |

Code cache growth during the first race (120-180 s): core 0 (the game's main thread) +4.9 MiB,
core 1 +4.2, core 2 +3.3; after it, almost none for the rest of the session.

So for Mario Kart the shader/pipeline path is **already** remembered and prepared at boot: the
first race creates 0-2 pipelines per 10 s and skips no draw. A track never played before makes its
pipelines while it loads (180 here), and those are in the cache from then on. What the first race
of a session does that later races don't is **translate the game's CPU code**: the race logic,
physics, AI, items and HUD code is translated the first time it runs, on the emulated cores
(core 0 being the game's main thread), and kept only in memory. That matches every point of the
report: worse in the first race only, better on every later track (the code is shared), back
after a relaunch.

Not everything in the dip is translation: the start of a race has twice the draws of later
moments (~800 a frame), and the game itself may do first-time work of its own that no emulator
cache can skip. The translation is the part that is Eden's own and measurable: ~13-17% of an
emulated core's time at the start, on the main thread among them.

The smaller first-time dips on scenery: the logs show new images (100-500 per 10 s) and, for
effects never seen before at all, new pipelines (made once, cached from then on).

## 4. What can be prepared during the black screen

- **Pipelines:** already all of them. The 9 s is mostly Eden translating every cached shader to
  SPIR-V again plus RADV looking each pipeline up in its cache (not measured separately yet).
  Caching the SPIR-V too would shorten the black screen, not the first race.
- **The CPU recompiler's code:** a persistent translation cache, loaded at boot (a few tens of
  MiB per core), each block checked against the game's code the first time it is needed. This is
  Ryujinx's PPTC idea (the comparison's item 5), being built for dynarmic: see
  `docs/ryujinx-comparison.md` and below.
- **Textures:** no: their contents come from the game's memory as it runs.

## 5. Changes proposed (smallest first)

1. **Instrumentation** (a test build first): the boot's shader cache load split into pipelines
   loaded, Eden's SPIR-V translation time, `vkCreate*Pipelines` time, and RADV's cache hits
   (`VK_EXT_pipeline_creation_feedback`); during play, per 10 s, the same for new pipelines; the
   recompiler per core (blocks, time). Confirms on the console what section 3 infers from
   counters, and shows whether `vulkan_pipelines.bin` is really hit at boot.
2. **Safer cache writes:** `vulkan_pipelines.bin` is written in place (truncate, then write): a
   crash or power cut during a write leaves a bad file. Write to a temporary file and rename.
3. **Persistent translation cache for dynarmic (A32):** saved when leaving the game, loaded at
   boot per game (title id), per core; rejected as a whole on another Eden build, another JIT
   configuration or another code cache address; each block used only if the game's code it was
   translated from (and its flags look-ahead) still hashes the same, translated again otherwise.
   State: implemented in dynarmic (`wip/pptc-dynarmic.patch`, worktree `~/eden-pptc`) and tested
   in the harness (`tools/dynarmic-harness/ctx.sh`, `cachetest`): ~800 random programs (ARM,
   Thumb, exclusives, control flow, flags, NEON), fastmem and split stacks, the PS4's translator
   options, a CP15 like Eden's, a cache saved and reloaded twice; the second run's objects all
   elsewhere and the first run's memory unmapped, so a pointer left unrelocated faults; a changed
   program's blocks all found stale (269 of 269) and translated again. In 04.17 (patches
   0104-0106, with 1 and 2).

## 6. On the console (04.18 logs) and 04.19

The warm runs loaded the caches (Mario Kart: 163,801 blocks on core 0 in 2.2 s; Echoes of Wisdom:
60,902 in 0.7 s) and used **none of them**: "0 of 163801, 352 stale". Eden's loader puts the game's
code region at a random 16 KiB-aligned offset of up to 16 MiB every boot
(`deconstructed_rom_directory.cpp`, "this is bad form of ASLR"), so every guest address differed from
the saved run's; the 352 "stale" blocks were addresses that happened to hold other code. The checks
did their job (nothing wrong was run), the cache did nothing.

04.19: the offset comes from the title id on the PS4 (the same every boot), the relocation scan
accepts only `mov r64, imm64` / moffs immediates (the old scan could have rewritten instruction
bytes that looked like a page table address), and a cache whose last run didn't end normally is
deleted instead of loaded. Test A / test B (cold, then warm) are still to be done on the console
with the cache really in use: the 30-second line's "translation cache blocks used so far" should
now be most of the loaded blocks, and "translating by core" near zero at the first race.

Echoes of Wisdom's slowness on its second and third runs was not the cache (README, 04.19).
