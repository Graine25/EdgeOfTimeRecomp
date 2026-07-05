/**
 * @file    gpu/device/host_heap_arena.h
 * @brief   O(1) coalescing arena for host-side guest-VA shadow structs.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Reserves one block in the guest physical window (vA0000000, offset 0) and
 * runs a single o1heap over it. Many small host shadow structs fragmented the
 * non-coalescing system heap ("BaseHeap::Alloc failed to find contiguous
 * range"); o1heap coalesces on free. Offset-0 pointers round-trip to valid
 * guest VAs and stay FromGuest-resolvable.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

namespace eot::gpu {

class HostHeapArena {
public:
  static HostHeapArena &Get();

  bool Init();

  void *Alloc(std::size_t size, std::size_t alignment);
  void Free(void *host_ptr);

  u32 AllocGuest(std::size_t size, std::size_t alignment);
  void FreeGuest(u32 guest_va);

  struct Snapshot {
    std::size_t capacity;
    std::size_t allocated;
    std::size_t peak_allocated;
    u64 oom_count;
    u64 live_count;
    bool ready;
  };
  Snapshot GetSnapshot();
};

}
