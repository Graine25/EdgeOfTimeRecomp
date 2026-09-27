#include <algorithm>
#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_MMWorkBuffer_Reset); // (buffer r3)
REX_EXTERN(eot_WorkBuf_Lock);              // (section r3, timeout r4)
REX_EXTERN(eot_WorkBuf_Unlock);            // (section r3); critical_section.cpp's release

namespace {

constexpr uint32_t kPrimaryData = 4;
constexpr uint32_t kPrimarySize = 12;
constexpr uint32_t kCurrentSize = 44;

constexpr uint32_t kHeapTable = 0x824E61F0;
constexpr uint32_t kMainHeapSlot = 4;
constexpr uint32_t kHeapLock = 12;
constexpr uint32_t kHeapLive = 32;
constexpr uint32_t kHeapFreeCells = 60;
constexpr uint32_t kSizeClasses = 12;
constexpr uint32_t kBlockNext = 0;
constexpr uint32_t kBlockSize = 12;
constexpr uint32_t kBlockSizeMask = 0x3FFFFFFF;
constexpr uint32_t kMaxBlocks = 1u << 20;

struct HeapCensus {
  uint32_t heap = 0;
  uint32_t live = 0;
  uint64_t free = 0;
  uint32_t largest = 0;
  uint32_t blocks = 0;
};

HeapCensus CensusMainHeap(const PPCContext &ctx, uint8_t *base) {
  HeapCensus census;
  const uint32_t table = eot::mem::load<uint32_t>(kHeapTable);
  if (!table)
    return census;
  census.heap = eot::mem::load<uint32_t>(table + kMainHeapSlot);
  if (!census.heap)
    return census;

  PPCContext call = ctx;
  call.r3.u32 = census.heap + kHeapLock;
  call.r4.u32 = 0xFFFFFFFFu;
  eot_WorkBuf_Lock(call, base);
  census.live = eot::mem::load<uint32_t>(census.heap + kHeapLive);
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

struct FailedRun {
  uint32_t buffer = 0;
  uint32_t frames = 0;
};
FailedRun g_runs[2];
uint32_t g_failed_total = 0;

FailedRun &RunOf(uint32_t buffer) {
  for (FailedRun &run : g_runs) {
    if (run.buffer == buffer)
      return run;
  }
  for (FailedRun &run : g_runs) {
    if (run.buffer == 0) {
      run.buffer = buffer;
      return run;
    }
  }
  return g_runs[1];
}

}

REX_HOOK_RAW(eot_MMWorkBuffer_Reset) {
  const uint32_t buffer = ctx.r3.u32;
  const PPCContext entry = ctx;
  __imp__eot_MMWorkBuffer_Reset(ctx, base);

  FailedRun &run = RunOf(buffer);
  const uint32_t wanted = eot::mem::load<uint32_t>(buffer + kPrimarySize);
  if (eot::mem::load<uint32_t>(buffer + kPrimaryData) != 0 || wanted == 0) {
    if (run.frames != 0) {
      EOT_INFO("[mem] frame command buffer {:#x}: its primary block is back ({} KB) after {} frame(s) in chunks",
               buffer, wanted / 1024, run.frames);
      run.frames = 0;
    }
    return;
  }

  eot::mem::store<uint32_t>(buffer + kPrimarySize, 0);
  eot::mem::store<uint32_t>(buffer + kCurrentSize, 0);
  ++g_failed_total;
  if (run.frames++ == 0) {
    const HeapCensus census = CensusMainHeap(entry, base);
    EOT_WARN("[mem] frame command buffer {:#x}: heap {:#x} has no free block of {} KB for it (in use {} KB, "
             "free {} KB in {} blocks, largest {} KB); building in 256 KB chunks until it has (failure {})",
             buffer, wanted / 1024, census.heap, census.live / 1024, census.free / 1024, census.blocks,
             census.largest / 1024, g_failed_total);
  }
}
