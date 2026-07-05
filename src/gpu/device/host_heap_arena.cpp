/**
 * @file    gpu/device/host_heap_arena.cpp
 * @brief   O(1) coalescing arena over a reserved guest-physical block.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device/host_heap_arena.h"

#include <mutex>

#include <o1heap.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "core/logging.h"
#include "core/profiling.h"

#if defined(REXGLUE_ENABLE_PROFILING)
#define EOT_MEM_ALLOC(ptr, size)                                                \
  do {                                                                         \
    if (TracyIsStarted)                                                        \
      TracyAllocN((ptr), (size), "HostResourceHeap");                          \
  } while (0)
#define EOT_MEM_FREE(ptr)                                                       \
  do {                                                                         \
    if (TracyIsStarted)                                                        \
      TracyFreeN((ptr), "HostResourceHeap");                                   \
  } while (0)
#define EOT_MEM_PLOT(name, val)                                                 \
  do {                                                                         \
    if (TracyIsStarted)                                                        \
      TracyPlot((name), static_cast<double>(val));                             \
  } while (0)
#else
#define EOT_MEM_ALLOC(ptr, size) ((void)0)
#define EOT_MEM_FREE(ptr) ((void)0)
#define EOT_MEM_PLOT(name, val) ((void)0)
#endif

namespace eot::gpu {

namespace {

constexpr u32 kArenaSize = 64u * 1024u * 1024u;

struct ArenaState {
  std::mutex mutex;
  O1HeapInstance *handle = nullptr;
  u32 guest_va = 0;
  void *host_base = nullptr;
  u64 live_count = 0;
  bool ready = false;
  bool oom_logged = false;
};

ArenaState &arena() {
  static ArenaState s;
  return s;
}

#if defined(REXGLUE_ENABLE_PROFILING)
void EmitPlotsLocked(const ArenaState &s) {
  const O1HeapDiagnostics d = o1heapGetDiagnostics(s.handle);
  EOT_MEM_PLOT("HostHeap Live Bytes", d.allocated);
  EOT_MEM_PLOT("HostHeap Peak Bytes", d.peak_allocated);
  EOT_MEM_PLOT("HostHeap Live Allocs", s.live_count);
  EOT_MEM_PLOT("HostHeap OOM Count", d.oom_count);
}
#else
void EmitPlotsLocked(const ArenaState &) {}
#endif

}

HostHeapArena &HostHeapArena::Get() {
  static HostHeapArena instance;
  return instance;
}

bool HostHeapArena::Init() {
  ArenaState &s = arena();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.ready)
    return true;

  auto *memory = REX_KERNEL_MEMORY();
  if (!memory) {
    EOT_ERROR("HostHeapArena::Init: no Memory instance");
    return false;
  }

  using namespace rex::memory;
  BaseHeap *heap =
      memory->LookupHeapByType(true, 64 * 1024);
  if (!heap) {
    EOT_ERROR(
        "HostHeapArena::Init: LookupHeapByType(physical, 64K) returned null");
    return false;
  }

  u32 guest_va = 0;
  const bool ok =
      heap->Alloc(kArenaSize, 64u * 1024u,
                  kMemoryAllocationReserve | kMemoryAllocationCommit,
                  kMemoryProtectRead | kMemoryProtectWrite,
                  true, &guest_va);
  if (!ok || !guest_va) {
    EOT_ERROR("HostHeapArena::Init: failed to reserve {} bytes in vA0000000",
             kArenaSize);
    return false;
  }

  void *host_base = memory->TranslateVirtual<void *>(guest_va);
  O1HeapInstance *handle = o1heapInit(host_base, kArenaSize);
  if (!handle) {
    EOT_ERROR("HostHeapArena::Init: o1heapInit failed (size {})", kArenaSize);
    return false;
  }

  s.handle = handle;
  s.guest_va = guest_va;
  s.host_base = host_base;
  s.ready = true;

#if defined(REXGLUE_ENABLE_PROFILING)
  if (TracyIsStarted) {
    TracyPlotConfig("HostHeap Live Bytes", tracy::PlotFormatType::Memory, true,
                    true, 0);
    TracyPlotConfig("HostHeap Peak Bytes", tracy::PlotFormatType::Memory, true,
                    true, 0);
    TracyPlotConfig("HostHeap Live Allocs", tracy::PlotFormatType::Number, true,
                    true, 0);
    TracyPlotConfig("HostHeap OOM Count", tracy::PlotFormatType::Number, true,
                    true, 0);
  }
#endif
  return true;
}

void *HostHeapArena::Alloc(std::size_t size, std::size_t alignment) {
  EOT_CPU_ZONE("HostHeap::Alloc");
  ArenaState &s = arena();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.ready) {
    EOT_ERROR("HostHeapArena::Alloc before Init");
    return nullptr;
  }
  if (size == 0)
    size = 1;
  if (alignment > O1HEAP_ALIGNMENT) {
    EOT_ERROR("HostHeapArena::Alloc alignment {} > {} unsupported", alignment,
             static_cast<std::size_t>(O1HEAP_ALIGNMENT));
    return nullptr;
  }

  void *p = o1heapAllocate(s.handle, size);
  if (!p) {
    if (!s.oom_logged) {
      const O1HeapDiagnostics d = o1heapGetDiagnostics(s.handle);
      EOT_ERROR("HostHeapArena OOM: req {} allocated {}/{} peak {} (raise "
               "kArenaSize)",
               size, d.allocated, d.capacity, d.peak_allocated);
      s.oom_logged = true;
    }
    EmitPlotsLocked(s);
    return nullptr;
  }

  ++s.live_count;
  EOT_MEM_ALLOC(p, size);
  EmitPlotsLocked(s);
  return p;
}

void HostHeapArena::Free(void *host_ptr) {
  if (!host_ptr)
    return;
  EOT_CPU_ZONE("HostHeap::Free");
  ArenaState &s = arena();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.ready)
    return;
  EOT_MEM_FREE(host_ptr);
  o1heapFree(s.handle, host_ptr);
  if (s.live_count)
    --s.live_count;
  EmitPlotsLocked(s);
}

u32 HostHeapArena::AllocGuest(std::size_t size, std::size_t alignment) {
  void *host = Alloc(size, alignment);
  if (!host)
    return 0;
  return REX_KERNEL_MEMORY()->HostToGuestVirtual(host);
}

void HostHeapArena::FreeGuest(u32 guest_va) {
  if (!guest_va)
    return;
  void *host = REX_KERNEL_MEMORY()->TranslateVirtual<void *>(guest_va);
  Free(host);
}

HostHeapArena::Snapshot HostHeapArena::GetSnapshot() {
  ArenaState &s = arena();
  std::lock_guard<std::mutex> lock(s.mutex);
  Snapshot out{};
  out.ready = s.ready;
  out.live_count = s.live_count;
  if (s.ready) {
    const O1HeapDiagnostics d = o1heapGetDiagnostics(s.handle);
    out.capacity = d.capacity;
    out.allocated = d.allocated;
    out.peak_allocated = d.peak_allocated;
    out.oom_count = d.oom_count;
  }
  return out;
}

}
