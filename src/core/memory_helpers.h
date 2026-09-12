#pragma once

#include <rex/system/kernel_state.h>
#include <rex/types.h>

namespace eot {

template <typename T> using be = rex::be<T>;

namespace mem {

template <typename T> inline T *at(u32 va) {
  auto *kernel = REX_KERNEL_STATE();
  auto *memory = kernel ? kernel->memory() : nullptr;
  if (!memory || va < 0x1000)
    return nullptr;
  return memory->template TranslateVirtual<T *>(va);
}

template <typename T> inline T *phys(u32 gpu_address) {
  auto *kernel = REX_KERNEL_STATE();
  auto *memory = kernel ? kernel->memory() : nullptr;
  if (!memory)
    return nullptr;
  return memory->template TranslatePhysical<T *>(gpu_address & 0x1FFFFFFFu);
}

template <typename T> inline T load(u32 va, T fallback = T{}) {
  auto *p = at<be<T>>(va);
  return p ? static_cast<T>(*p) : fallback;
}

template <typename T> inline bool store(u32 va, T value) {
  auto *p = at<be<T>>(va);
  if (!p)
    return false;
  *p = value;
  return true;
}

inline u32 u32at(u32 va) { return load<u32>(va); }
inline f32 f32at(u32 va) { return load<f32>(va); }

inline bool readable(u32 va, u32 bytes) {
  auto *kernel = REX_KERNEL_STATE();
  auto *memory = kernel ? kernel->memory() : nullptr;
  if (!memory || va < 0x1000 || bytes == 0 || va + bytes < va)
    return false;
  u32 cursor = va;
  const u32 end = va + bytes;
  while (cursor < end) {
    auto *heap = memory->LookupHeap(cursor);
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(cursor, &info))
      return false;
    if (!(info.state & rex::memory::kMemoryAllocationCommit) ||
        info.protect == rex::memory::kMemoryProtectNoAccess)
      return false;
    const u32 region_end = info.base_address + info.region_size;
    if (region_end <= cursor)
      return false;
    cursor = region_end;
  }
  return true;
}

inline const char *str(u32 va) {
  auto *p = at<const char>(va);
  return p ? p : "";
}

}
}
