#include <chrono>
#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_PhysicsWorld_Interpolate);

namespace {

constexpr uint32_t kPhysicsWorld = 0x824A11B8;
constexpr uint32_t kWorldObjectList = kPhysicsWorld;
constexpr uint32_t kWorldStepsThisFrame = kPhysicsWorld + 1788;

uint64_t g_frames = 0, g_steps = 0, g_zero_step_frames = 0;
double g_alpha_sum = 0.0;

void LogSummary() {
  using clock = std::chrono::steady_clock;
  static clock::time_point last = clock::now();
  const auto now = clock::now();
  if (now - last < std::chrono::seconds(30))
    return;
  last = now;
  if (g_frames)
    EOT_INFO("[physics] last 30 s: {} frames, {} fixed steps, {} frames with no step ({:.0f}%), mean alpha "
             "{:.3f}, {} bodies",
             g_frames, g_steps, g_zero_step_frames,
             100.0 * static_cast<double>(g_zero_step_frames) / static_cast<double>(g_frames),
             g_alpha_sum / static_cast<double>(g_frames),
             eot::mem::load<uint32_t>(eot::mem::load<uint32_t>(kWorldObjectList) + 4));
  g_frames = g_steps = g_zero_step_frames = 0;
  g_alpha_sum = 0.0;
}

}

REX_HOOK_RAW(eot_PhysicsWorld_Interpolate) {
  const uint32_t steps = eot::mem::load<uint32_t>(kWorldStepsThisFrame);
  ++g_frames;
  g_steps += steps;
  if (steps == 0)
    ++g_zero_step_frames;
  g_alpha_sum += ctx.f1.f64;
  __imp__eot_PhysicsWorld_Interpolate(ctx, base);
  LogSummary();
}
