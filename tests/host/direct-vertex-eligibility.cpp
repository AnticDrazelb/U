// Host test of Direct Vertex Data's eligibility tracking (LatteBufferCache.cpp, the block between
// "Crema setting direct_vertex (Experimental)" and its #endif): run.sh extracts that block from
// the Cemu source into direct_vertex_block.inc and builds this against it.
#include <atomic>
#include <memory>
#include <algorithm>
#include <cstdint>
#include <cstdio>
typedef uint32_t uint32; typedef uint64_t uint64; typedef uint32_t MPTR;
#include "direct_vertex_block.inc"

void LatteBufferCache_notifySwapTVScanBuffer() { s_cremaDirectFrame.fetch_add(1, std::memory_order_relaxed); }
void frames(int n) { for (int i = 0; i < n; i++) LatteBufferCache_notifySwapTVScanBuffer(); }
int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
  const MPTR base = 0x10000000; const uint32 size = 0x40000000;
  // before init: nothing usable, marks are ignored
  g_crema_direct_vertex = 1;
  CHECK(!LatteBufferCache_CremaDirectUsable(base, 64));
  CremaDirectMark(base, 64, false);
  LatteBufferCache_CremaDirectInit(base, size);
  // static: just initialised = just written
  CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x1000, 0x100));
  frames(3); CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x1000, 0x100));
  frames(1); CHECK(LatteBufferCache_CremaDirectUsable(base + 0x1000, 0x100));
  // out of range: below, straddling the end, past the end, zero size
  CHECK(!LatteBufferCache_CremaDirectUsable(base - 0x100, 0x200));
  CHECK(!LatteBufferCache_CremaDirectUsable(base + size - 0x100, 0x200));
  CHECK(LatteBufferCache_CremaDirectUsable(base + size - 0x100, 0x100));
  CHECK(!LatteBufferCache_CremaDirectUsable(0xF4000000, 0x100)); // MEM1
  CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x1000, 0));
  // a CPU write in one 64 KiB block: that block and any range touching it, for 4 frames
  CremaDirectMark(base + 0x25000, 0x10, false);
  CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x20000, 0x10));
  CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x10000, 0x20000)); // spans into it
  CHECK(LatteBufferCache_CremaDirectUsable(base + 0x10000, 0x10000));  // the block before
  CHECK(LatteBufferCache_CremaDirectUsable(base + 0x30000, 0x10000));  // the block after
  frames(4); CHECK(LatteBufferCache_CremaDirectUsable(base + 0x20000, 0x10));
  // a write range spanning blocks marks all of them, clipped to the imported range
  CremaDirectMark(base - 0x8000, 0x20000, false); // from below MEM2 into block 0 and 1
  CHECK(!LatteBufferCache_CremaDirectUsable(base, 0x10)); CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x10000, 0x10));
  CHECK(LatteBufferCache_CremaDirectUsable(base + 0x20000, 0x10));
  CremaDirectMark(base + size - 0x10, 0x1000, false); // past the end: no overflow
  CHECK(!LatteBufferCache_CremaDirectUsable(base + size - 0x10, 0x10));
  frames(4);
  // all-data mode ignores CPU writes...
  g_crema_direct_vertex = 2; CremaDirectMark(base + 0x50000, 0x10, false);
  CHECK(LatteBufferCache_CremaDirectUsable(base + 0x50000, 0x10));
  // ...but never streamout output, in either mode, until the CPU writes there again
  CremaDirectMark(base + 0x60000, 0x100, true);
  CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x60000, 0x10));
  g_crema_direct_vertex = 1; frames(100);
  CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x60000, 0x10));
  CremaDirectMark(base + 0x60000, 0x10, false); frames(4);
  CHECK(LatteBufferCache_CremaDirectUsable(base + 0x60000, 0x10));
  // a global flush (address 0 or size ~0) marks everything
  CremaDirectMark(0, 0xFFFFFFFF, false);
  CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x3000000, 0x10));
  frames(4); CHECK(LatteBufferCache_CremaDirectUsable(base + 0x3000000, 0x10));
  // setting off: never
  g_crema_direct_vertex = 0; CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x3000000, 0x10));
  // frame counter wrap-around keeps working (unsigned difference)
  g_crema_direct_vertex = 1;
  s_cremaDirectFrame.store(0xFFFFFFF0u); CremaDirectMark(base + 0x70000, 0x10, false);
  frames(3); CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x70000, 0x10));
  frames(17); CHECK(LatteBufferCache_CremaDirectUsable(base + 0x70000, 0x10));
  // a second game start in the same process: same table, everything "just written" again
  g_crema_direct_vertex = 1; frames(10);
  CHECK(LatteBufferCache_CremaDirectUsable(base + 0x3000000, 0x10));
  std::atomic<uint32>* table = s_cremaDirectWrites.get();
  LatteBufferCache_CremaDirectInit(base, size);
  CHECK(s_cremaDirectWrites.get() == table);
  CHECK(!LatteBufferCache_CremaDirectUsable(base + 0x3000000, 0x10));
  frames(4); CHECK(LatteBufferCache_CremaDirectUsable(base + 0x3000000, 0x10));
  printf("%s (%d failures)\n", fails ? "FAILED" : "all checks passed", fails);
  return fails != 0;
}
