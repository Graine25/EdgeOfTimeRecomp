#pragma once

#include <rex/system/kernel_state.h>
#include <rex/types.h>

namespace eot {

template <typename T> using be = rex::be<T>;

namespace mem {

template <typename T> inline T *at(u32 va) {
  auto *memory = REX_KERNEL_MEMORY();
  if (!memory || !va)
    return nullptr;
  return memory->template TranslateVirtual<T *>(va);
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

inline const char *str(u32 va) {
  auto *p = at<const char>(va);
  return p ? p : "";
}

bool ready();

void *try_translate(u32 va, u32 align);

template <typename T> inline T *try_at(u32 va) {
  return static_cast<T *>(try_translate(va, alignof(T)));
}

template <typename T> inline T try_load(u32 va, T fallback = T{}) {
  auto *p = try_at<const be<T>>(va);
  return p ? static_cast<T>(*p) : fallback;
}

template <typename T> inline bool try_store(u32 va, T value) {
  auto *p = try_at<be<T>>(va);
  if (!p)
    return false;
  *p = value;
  return true;
}

template <typename T> inline T try_field(u32 base, u32 off, T fallback = T{}) {
  return base ? try_load<T>(base + off, fallback) : fallback;
}

template <typename T> struct GuestVec {
  be<u32> first;
  be<u32> last;
  be<u32> cap;

  bool empty() const {
    return !static_cast<u32>(first) ||
           static_cast<u32>(first) == static_cast<u32>(last);
  }

  u32 size() const {
    return empty() ? 0u
                   : (static_cast<u32>(last) - static_cast<u32>(first)) /
                         static_cast<u32>(sizeof(T));
  }

  u32 address(u32 index) const {
    return static_cast<u32>(first) + index * static_cast<u32>(sizeof(T));
  }

  T operator[](u32 index) const {
    return index < size() ? load<T>(address(index)) : T{};
  }
};
static_assert(sizeof(GuestVec<u32>) == 0x0C);

}
}
