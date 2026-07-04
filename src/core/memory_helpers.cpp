#include "core/memory_helpers.h"

#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace eot::mem {

bool ready() {
  auto *rt = rex::Runtime::instance();
  return rt && rt->memory();
}

void *try_translate(u32 va, u32 align) {
  auto *rt = rex::Runtime::instance();
  auto *memory = rt ? rt->memory() : nullptr;
  if (!memory || !va || (va & (align - 1)))
    return nullptr;

  auto *heap = memory->LookupHeap(va);
  u32 protect = 0;
  if (!heap || !heap->QueryProtect(va, &protect) ||
      !(protect & rex::memory::kMemoryProtectRead)) {
    return nullptr;
  }
  return memory->TranslateVirtual<void *>(va);
}

}
