# native-test

`run.sh [eden-tree]`: the native memcpy/memmove/memset/memcmp (Eden's
`src/core/arm/dynarmic/native_functions.h`, `Move`, `Fill` and (04.80) `Compare`) against
`memmove`/`memset`/`memcmp` on a flat buffer (memcmp: its sign, and the bytes' difference as the SDK
returns it): 16 pages, each plain (a host pointer) or not (memory the GPU caches: only `ReadBlock` and
`WriteBlock`), 200,000 random copies, fills and compares, overlapping half the time.

04.75-04.77 copied the pieces that touch a non-plain page with `Memory::CopyBlock`, which walked the
destination and copied each piece onto itself: nothing was copied (22% of these runs wrong; Echoes of
Wisdom's flicker and its crash on a garbage index buffer). 04.78 copies them through a page buffer
with `ReadBlock` and `WriteBlock` (and fixes `CopyBlock`): none wrong.
