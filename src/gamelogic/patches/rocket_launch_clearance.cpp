#include <cstdint>
#include <unordered_map>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_SiegeRocket_HandleMessage);
REX_EXTERN(__imp__eot_SiegeRocket_Explode);

namespace {

constexpr int32_t kContactMessage = 7;
constexpr int32_t kForceFieldMessage = 32;
constexpr uint32_t kAttackerOffset = 128;
constexpr uint32_t kFlagsOffset = 172;
constexpr uint32_t kLifetimeOffset = 184;
constexpr float kLaunchLifetime = 8.0f;
constexpr uint32_t kAttackState = 2;

constexpr float kClearance = 2.0f / 60.0f;
constexpr float kGap = 4.0f / 60.0f;

constexpr uint32_t kMaxLines = 80;

struct Launch {
  float last_ignored = 0.0f;
  uint32_t ignored = 0;
  bool open = false;
};

std::unordered_map<uint32_t, Launch> g_launches;
uint32_t g_lines = 0;

bool MayLog() { return ++g_lines <= kMaxLines; }

uint32_t CurrentState(uint32_t obj) {
  const uint32_t block = eot::mem::load<uint8_t>(obj + 18) == 1
                             ? obj + 20
                             : 16u * eot::mem::load<uint8_t>(obj + 19) + eot::mem::load<uint32_t>(obj + 20);
  return eot::mem::load<uint16_t>(block);
}

float Flight(uint32_t rocket) { return kLaunchLifetime - eot::mem::load<float>(rocket + kLifetimeOffset); }

void CloseLaunch(uint32_t rocket, const char *why) {
  auto it = g_launches.find(rocket);
  if (it == g_launches.end() || !it->second.open)
    return;
  it->second.open = false;
  if (MayLog())
    EOT_INFO("[rocket] launch overlap: {} attacker contacts dropped, the last {:.4f} s into flight, then {} (rocket {:#x})",
             it->second.ignored, kLaunchLifetime - it->second.last_ignored, why, rocket);
}

bool LaunchOverlap(uint32_t rocket, float lifetime) {
  Launch &launch = g_launches[rocket];
  const bool fresh = !launch.open || lifetime > launch.last_ignored;
  if (fresh) {
    CloseLaunch(rocket, "a new launch");
    if (lifetime <= kLaunchLifetime - kClearance)
      return false;
    launch = Launch{};
    launch.open = true;
  } else if (launch.last_ignored - lifetime > kGap) {
    CloseLaunch(rocket, "a gap");
    return false;
  }
  launch.last_ignored = lifetime;
  ++launch.ignored;
  EOT_DEBUG("[rocket] launch overlap: dropped attacker contact {:.4f} s into flight (rocket {:#x})",
            kLaunchLifetime - lifetime, rocket);
  return true;
}

}

REX_HOOK_RAW(eot_SiegeRocket_HandleMessage) {
  const uint32_t rocket = ctx.r3.u32;
  const int32_t message = ctx.r4.s32;
  const uint32_t payload = ctx.r5.u32;
  if (message == kContactMessage && payload && CurrentState(rocket) == kAttackState) {
    const uint32_t touched = eot::mem::load<uint32_t>(payload + 12);
    const uint32_t attacker = eot::mem::load<uint32_t>(rocket + kAttackerOffset);
    const float lifetime = eot::mem::load<float>(rocket + kLifetimeOffset);
    if (touched == attacker && LaunchOverlap(rocket, lifetime)) {
      ctx.r3.u32 = 0;
      return;
    }
    CloseLaunch(rocket, touched == attacker ? "a later attacker contact" : "a contact with another object");
    if (MayLog())
      EOT_INFO("[rocket] contact with {:#x}{} {:.4f} s into flight reached the game (rocket {:#x})", touched,
               touched == attacker ? " (the attacker)" : "", kLaunchLifetime - lifetime, rocket);
  } else if (message == kForceFieldMessage && payload && MayLog()) {
    EOT_INFO("[rocket] force-field contact, object type {}, {:.4f} s into flight (rocket {:#x}, state {})",
             eot::mem::load<uint32_t>(payload + 20), Flight(rocket), rocket, CurrentState(rocket));
  }
  __imp__eot_SiegeRocket_HandleMessage(ctx, base);
}

REX_HOOK_RAW(eot_SiegeRocket_Explode) {
  const uint32_t rocket = ctx.r3.u32;
  CloseLaunch(rocket, "the explosion");
  if (MayLog())
    EOT_INFO("[rocket] explode {:.4f} s into flight (rocket {:#x}, flags {:#x}, state {})", Flight(rocket), rocket,
             eot::mem::load<uint8_t>(rocket + kFlagsOffset), CurrentState(rocket));
  __imp__eot_SiegeRocket_Explode(ctx, base);
}
