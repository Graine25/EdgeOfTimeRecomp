#include "goliath/debug/occlusion.h"

#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/debug/freecam.h"

REX_EXTERN(__imp__eot_Renderer_UpdateViewMatrix);

REXCVAR_DEFINE_STRING(
    eot_occlusion, "auto", "EdgeOfTime/Debug",
    "The engine's CPU occluder culling. \"auto\" holds it off while the debug free camera is "
    "flying and leaves it alone otherwise; \"on\" always leaves it alone; \"off\" holds it off "
    "always. The authored occluder quads assume the eye is where the game puts it, so a camera "
    "somewhere else can find itself behind one and lose the whole room.");

namespace {

constexpr uint32_t kOccluderList = 0x824D7534;
constexpr uint32_t kCapacity = 4;
constexpr uint32_t kSlotsUsed = 8;
constexpr uint32_t kLiveCount = 12;

std::atomic<bool> g_suppressed{false};

bool Wanted() {
  const std::string mode = rex::cvar::GetFlagByName("eot_occlusion");
  if (mode == "off")
    return true;
  if (mode == "on")
    return false;
  return eot::debug::FreecamActive();
}

}

namespace eot::debug {

bool OcclusionSuppressed() { return g_suppressed.load(std::memory_order_relaxed); }

}

REX_HOOK_RAW(eot_Renderer_UpdateViewMatrix) {
  const bool hold = Wanted();
  if (hold != g_suppressed.exchange(hold, std::memory_order_relaxed))
    EOT_INFO("[occlusion] occluder culling {}", hold ? "held off" : "back on");

  if (!hold) {
    __imp__eot_Renderer_UpdateViewMatrix(ctx, base);
    return;
  }

  const uint32_t capacity = eot::mem::load<uint32_t>(kOccluderList + kCapacity);
  const uint32_t slots = eot::mem::load<uint32_t>(kOccluderList + kSlotsUsed);
  const uint32_t live = eot::mem::load<uint32_t>(kOccluderList + kLiveCount);
  eot::mem::store<uint32_t>(kOccluderList + kCapacity, 0u);
  eot::mem::store<uint32_t>(kOccluderList + kSlotsUsed, 0u);
  eot::mem::store<uint32_t>(kOccluderList + kLiveCount, 0u);

  __imp__eot_Renderer_UpdateViewMatrix(ctx, base);

  eot::mem::store<uint32_t>(kOccluderList + kCapacity, capacity);
  eot::mem::store<uint32_t>(kOccluderList + kSlotsUsed, slots);
  eot::mem::store<uint32_t>(kOccluderList + kLiveCount, live);
}
