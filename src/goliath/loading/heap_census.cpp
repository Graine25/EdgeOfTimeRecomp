#include "goliath/loading/heap_census.h"

#include <algorithm>
#include <chrono>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(eot_WorkBuf_Lock);   // (section r3, timeout r4)
REX_EXTERN(eot_WorkBuf_Unlock);

namespace eot::loading {
namespace {

constexpr uint32_t kHeapTable = 0x824E61F0;
constexpr uint32_t kMainHeapSlot = 4;
constexpr uint32_t kHeapLock = 12;
constexpr uint32_t kHeapInUse = 32;
constexpr uint32_t kHeapFreeCells = 60;
constexpr uint32_t kSizeClasses = 12;
constexpr uint32_t kBlockNext = 0;
constexpr uint32_t kBlockSize = 12;
constexpr uint32_t kBlockSizeMask = 0x3FFFFFFF;
constexpr uint32_t kMaxBlocks = 1u << 20;
constexpr auto kInterval = std::chrono::seconds(30);

struct Census {
  uint32_t heap = 0;
  uint32_t in_use = 0;
  uint64_t free = 0;
  uint32_t largest = 0;
  uint32_t blocks = 0;
};

Census Walk(const PPCContext &ctx, uint8_t *base) {
  Census census;
  const uint32_t table = eot::mem::load<uint32_t>(kHeapTable);
  census.heap = table ? eot::mem::load<uint32_t>(table + kMainHeapSlot) : 0;
  if (!census.heap)
    return census;

  PPCContext call = ctx;
  call.r3.u32 = census.heap + kHeapLock;
  call.r4.u32 = 0xFFFFFFFFu;
  eot_WorkBuf_Lock(call, base);
  census.in_use = eot::mem::load<uint32_t>(census.heap + kHeapInUse);
  for (uint32_t size_class = 0; size_class < kSizeClasses && census.blocks < kMaxBlocks; ++size_class) {
    const uint32_t cell = eot::mem::load<uint32_t>(census.heap + kHeapFreeCells + 4 * size_class);
    for (uint32_t block = cell ? eot::mem::load<uint32_t>(cell) : 0; block && census.blocks < kMaxBlocks;
         block = eot::mem::load<uint32_t>(block + kBlockNext)) {
      const uint32_t size = eot::mem::load<uint32_t>(block + kBlockSize) & kBlockSizeMask;
      census.free += size;
      census.largest = std::max(census.largest, size);
      ++census.blocks;
    }
  }
  call = ctx;
  call.r3.u32 = census.heap + kHeapLock;
  eot_WorkBuf_Unlock(call, base);
  return census;
}

uint64_t FreePhysicalBytes() {
  auto *kernel = REX_KERNEL_STATE();
  auto *memory = kernel ? kernel->memory() : nullptr;
  auto *physical = memory ? memory->GetPhysicalHeap() : nullptr;
  return physical ? uint64_t(physical->GetUnreservedPageCount()) * physical->page_size() : 0;
}

std::chrono::steady_clock::time_point g_next{};

}

void HeapCensusTick(const PPCContext &ctx, uint8_t *base) {
  const auto now = std::chrono::steady_clock::now();
  if (now < g_next)
    return;
  g_next = now + kInterval;
  const Census census = Walk(ctx, base);
  if (!census.heap)
    return;
  EOT_INFO("[mem] heap 0: {} MB in use, {} MB free in {} blocks (largest {} KB); {} MB of physical memory "
           "outside it",
           census.in_use >> 20, census.free >> 20, census.blocks, census.largest >> 10, FreePhysicalBytes() >> 20);
}

}
