#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>

#include <rex/hook.h>

#include "core/memory_helpers.h"
#include "goliath/debug/scene_pause.h"
#include "gpu/settings.h"

REX_EXTERN(__imp__eot_GEEngineMgrBC_UpdateFrameTime);

namespace {

constexpr uint32_t kFixedFrameTimeFlag = 0x824E5A83;
constexpr uint32_t kFrameDeltaSeconds = 0x824E5A8C;

constexpr double kMinDelta = 1.0 / 1000.0;
constexpr double kMaxDelta = 0.1;

bool UnlockWanted() {
  const int32_t fps = eot::gpu::Settings::FpsLimit();
  return fps == 0 || fps > 60;
}

}

REX_HOOK_RAW(eot_GEEngineMgrBC_UpdateFrameTime) {
  eot::debug::ScenePauseTick();

  if (!UnlockWanted() || eot::mem::load<uint8_t>(kFixedFrameTimeFlag) != 0) {
    __imp__eot_GEEngineMgrBC_UpdateFrameTime(ctx, base);
    return;
  }

  using clock = std::chrono::steady_clock;
  static clock::time_point last{};
  const clock::time_point now = clock::now();
  double delta = (last.time_since_epoch().count() != 0)
                     ? std::chrono::duration<double>(now - last).count()
                     : 1.0 / 60.0;
  last = now;
  delta = std::clamp(delta, kMinDelta, kMaxDelta);

  eot::mem::store<uint32_t>(kFrameDeltaSeconds,
                            std::bit_cast<uint32_t>(static_cast<float>(delta)));
  ctx.f1.f64 = delta;
}
