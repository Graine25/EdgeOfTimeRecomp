#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_SiegeRocket_HandleMessage);

namespace {

constexpr int32_t kContactMessage = 7;
constexpr uint32_t kAttackerOffset = 128;
constexpr uint32_t kLifetimeOffset = 184;
constexpr float kLaunchLifetime = 8.0f;
constexpr float kClearance = 2.0f / 60.0f;
constexpr uint32_t kAttackState = 2;

uint32_t g_ignored = 0;

uint32_t CurrentState(uint32_t obj) {
  const uint32_t block = eot::mem::load<uint8_t>(obj + 18) == 1
                             ? obj + 20
                             : 16u * eot::mem::load<uint8_t>(obj + 19) + eot::mem::load<uint32_t>(obj + 20);
  return eot::mem::load<uint16_t>(block);
}

}

REX_HOOK_RAW(eot_SiegeRocket_HandleMessage) {
  const uint32_t rocket = ctx.r3.u32;
  const uint32_t payload = ctx.r5.u32;
  if (ctx.r4.s32 == kContactMessage && payload) {
    const uint32_t touched = eot::mem::load<uint32_t>(payload + 12);
    const float lifetime = eot::mem::load<float>(rocket + kLifetimeOffset);
    if (touched == eot::mem::load<uint32_t>(rocket + kAttackerOffset) && lifetime > kLaunchLifetime - kClearance &&
        CurrentState(rocket) == kAttackState) {
      if (++g_ignored <= 20)
        EOT_INFO("[rocket] launch clearance: ignored gunner contact {:.4f} s into flight (rocket {:#x}, gunner {:#x})",
                 kLaunchLifetime - lifetime, rocket, touched);
      ctx.r3.u32 = 0;
      return;
    }
  }
  __imp__eot_SiegeRocket_HandleMessage(ctx, base);
}
