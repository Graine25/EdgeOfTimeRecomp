#include <atomic>
#include <cstdint>
#include <mutex>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_GRHandleTable_AllocHandle);
REX_EXTERN(__imp__eot_HUDMgrBC_GetWin);
REX_EXTERN(__imp__eot_HUDMgrBC_DestroyWindowTree);

namespace {

constexpr uint32_t kHudMgr = 0x824A9928;
constexpr uint32_t kHudTable = kHudMgr + 4;
constexpr uint32_t kRetailSlots = 1024;
constexpr uint32_t kSlots = 4096;
constexpr uint32_t kExtraSlots = kSlots - kRetailSlots;

constexpr uint32_t kRuntimeBit = 0x80000000u;
constexpr uint32_t kNoHandle = 0xFFFFFFFFu;
constexpr uint32_t kWindowFlagsOffset = 52;
constexpr uint32_t kWindowHandleOffset = 60;
constexpr uint32_t kFlagRuntime = 0x80;

uint32_t SlotOf(uint32_t handle) { return (handle >> 16) & 0x7FFF; }
uint32_t GenerationOf(uint32_t handle) { return handle & 0xFFFF; }
bool Ours(uint32_t slot) { return slot >= kRetailSlots && slot < kSlots; }

uint64_t Pack(uint32_t generation, uint32_t window) { return (uint64_t(generation) << 32) | window; }
uint32_t GenerationIn(uint64_t entry) { return uint32_t(entry >> 32) & 0xFFFF; }
uint32_t WindowIn(uint64_t entry) { return uint32_t(entry); }

struct Overflow {
  std::atomic<uint64_t> entries[kExtraSlots];
  std::mutex lock;
  uint32_t hint = 0;
  uint32_t used = 0;
  uint32_t peak = 0;
  bool announced = false;
  bool exhausted = false;

  Overflow() {
    for (auto &entry : entries)
      entry.store(Pack(1, 0), std::memory_order_relaxed);
  }

  uint32_t Alloc(uint32_t window) {
    std::lock_guard<std::mutex> guard(lock);
    for (uint32_t n = 0; n < kExtraSlots; ++n) {
      const uint32_t index = (hint + n) % kExtraSlots;
      const uint64_t entry = entries[index].load(std::memory_order_relaxed);
      if (WindowIn(entry))
        continue;
      entries[index].store(Pack(GenerationIn(entry), window), std::memory_order_release);
      hint = (index + 1) % kExtraSlots;
      ++used;
      if (used > peak) {
        peak = used;
        if (!announced || (peak % 512) == 0) {
          EOT_INFO("[hud] the console's {} runtime window handles are all in use; {} more in the port's "
                   "table (peak {})",
                   kRetailSlots, used, peak);
          announced = true;
        }
      }
      return kRuntimeBit | ((kRetailSlots + index) << 16) | GenerationIn(entry);
    }
    if (!exhausted) {
      EOT_WARN("[hud] all {} runtime window handles are in use; the retail code will write through "
               "a null window next",
               kSlots);
      exhausted = true;
    }
    return 0;
  }

  uint32_t Resolve(uint32_t slot, uint32_t generation) const {
    const uint64_t entry = entries[slot - kRetailSlots].load(std::memory_order_acquire);
    return GenerationIn(entry) == generation ? WindowIn(entry) : 0;
  }

  void Free(uint32_t slot, uint32_t generation, uint32_t window) {
    std::lock_guard<std::mutex> guard(lock);
    std::atomic<uint64_t> &slot_entry = entries[slot - kRetailSlots];
    const uint64_t entry = slot_entry.load(std::memory_order_relaxed);
    if (GenerationIn(entry) != generation || WindowIn(entry) != window) {
      EOT_WARN("[hud] window {:#x} frees handle slot {} generation {} that holds window {:#x} generation {}",
               window, slot, generation, WindowIn(entry), GenerationIn(entry));
      return;
    }
    uint32_t next = generation + 1;
    if (next == 0x10000)
      next = 1;
    slot_entry.store(Pack(next, 0), std::memory_order_release);
    --used;
  }
};

Overflow &Table() {
  static Overflow table;
  return table;
}

}

REX_HOOK_RAW(eot_GRHandleTable_AllocHandle) {
  const uint32_t table = ctx.r3.u32;
  const uint32_t window = ctx.r4.u32;
  __imp__eot_GRHandleTable_AllocHandle(ctx, base);
  if (ctx.r3.u32 == 0 && table == kHudTable)
    ctx.r3.u32 = Table().Alloc(window);
}

REX_HOOK_RAW(eot_HUDMgrBC_GetWin) {
  const uint32_t handle = ctx.r3.u32;
  if ((handle & kRuntimeBit) && handle != kNoHandle) {
    const uint32_t slot = SlotOf(handle);
    if (Ours(slot)) {
      ctx.r3.u32 = Table().Resolve(slot, GenerationOf(handle));
      return;
    }
  }
  __imp__eot_HUDMgrBC_GetWin(ctx, base);
}

REX_HOOK_RAW(eot_HUDMgrBC_DestroyWindowTree) {
  const uint32_t window = ctx.r4.u32;
  if (window && (eot::mem::load<uint32_t>(window + kWindowFlagsOffset) & kFlagRuntime)) {
    const uint32_t handle = eot::mem::load<uint32_t>(window + kWindowHandleOffset);
    const uint32_t slot = SlotOf(handle);
    if (Ours(slot)) {
      Table().Free(slot, GenerationOf(handle), window);
      eot::mem::store<uint32_t>(window + kWindowHandleOffset, kRuntimeBit);
    }
  }
  __imp__eot_HUDMgrBC_DestroyWindowTree(ctx, base);
}
