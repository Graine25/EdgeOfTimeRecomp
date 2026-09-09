#include <bit>
#include <chrono>
#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_PhysicsWorld_Update);
REX_EXTERN(__imp__eot_PhysicsWorld_Interpolate);

namespace {

constexpr uint32_t kPhysicsWorld = 0x824A11B8;
constexpr uint32_t kWorldObjectList = kPhysicsWorld;
constexpr uint32_t kWorldStepSeconds = kPhysicsWorld + 1780;
constexpr uint32_t kWorldStepsThisFrame = kPhysicsWorld + 1788;
constexpr uint32_t kWorldAccumulator = kPhysicsWorld + 1800;
constexpr uint32_t kTimeScale = 0x824E56D0;

constexpr float kRetailStep = 1.0f / 60.0f;
constexpr float kStepShrink = 1.0f - 1.0f / 1048576.0f;

bool g_frame_stepped = false;

uint64_t g_frames = 0, g_frame_stepped_frames = 0, g_zero_step_frames = 0;
double g_alpha_sum = 0.0;

void LogSummary() {
  using clock = std::chrono::steady_clock;
  static clock::time_point last = clock::now();
  const auto now = clock::now();
  if (now - last < std::chrono::seconds(30))
    return;
  last = now;
  if (g_frames)
    EOT_INFO("[physics] last 30 s: {} frames, {} stepped as one frame ({:.0f}%), {} with no step, "
             "mean alpha {:.3f}, {} objects",
             g_frames, g_frame_stepped_frames,
             100.0 * static_cast<double>(g_frame_stepped_frames) / static_cast<double>(g_frames),
             g_zero_step_frames, g_alpha_sum / static_cast<double>(g_frames),
             eot::mem::load<uint32_t>(eot::mem::load<uint32_t>(kWorldObjectList) + 4));
  g_frames = g_frame_stepped_frames = g_zero_step_frames = 0;
  g_alpha_sum = 0.0;
}

}

REX_HOOK_RAW(eot_PhysicsWorld_Update) {
  const float scale = eot::mem::load<float>(kTimeScale);
  const float delta = static_cast<float>(ctx.f1.f64);
  float step = kRetailStep;
  g_frame_stepped = false;
  if (scale > 0.0f && delta > 0.0f) {
    const float frame = delta / scale;
    if (frame < kRetailStep) {
      step = frame * kStepShrink;
      g_frame_stepped = true;
    }
  }
  eot::mem::store<uint32_t>(kWorldStepSeconds, std::bit_cast<uint32_t>(step));
  __imp__eot_PhysicsWorld_Update(ctx, base);
  if (g_frame_stepped)
    eot::mem::store<uint32_t>(kWorldAccumulator, 0);
  LogSummary();
}

REX_HOOK_RAW(eot_PhysicsWorld_Interpolate) {
  ++g_frames;
  if (g_frame_stepped)
    ++g_frame_stepped_frames;
  if (eot::mem::load<uint32_t>(kWorldStepsThisFrame) == 0)
    ++g_zero_step_frames;
  if (g_frame_stepped)
    ctx.f1.f64 = 0.0;
  g_alpha_sum += ctx.f1.f64;
  __imp__eot_PhysicsWorld_Interpolate(ctx, base);
}
