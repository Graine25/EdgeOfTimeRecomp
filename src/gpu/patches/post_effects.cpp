#include <atomic>
#include <bit>
#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/settings.h"

REX_EXTERN(__imp__eot_RenderComposition_ExecuteChain);
REX_EXTERN(__imp__eot_GLAPICamera_SetFOVAngle);
REX_EXTERN(__imp__eot_GLAPICamera_GetFOVAngle);

namespace {

using eot::gpu::Settings;

constexpr uint32_t kPostFxDepthOfField = 1u << 1;
constexpr uint32_t kPostFxMotionBlur = 1u << 2;
constexpr uint32_t kPostFxRadialBlur = 1u << 3;
constexpr uint32_t kPostFxHeatVision = 1u << 5;
constexpr uint32_t kPostFxHeatHaze = 1u << 6;
constexpr uint32_t kPostFxColorization = 1u << 8;
constexpr uint32_t kPostFxGrain = 1u << 10;
constexpr uint32_t kPostFxEdgeFilter = 1u << 11;
constexpr uint32_t kPostFxHalo = 1u << 12;
constexpr uint32_t kPostFxCameraMotionBlur = 1u << 14;
constexpr uint32_t kPostFxBokeh = 1u << 15;

constexpr uint32_t kCommandFlags = 288;
constexpr uint32_t kCommandBloomEnable = 21;

uint32_t DisabledStages() {
  uint32_t mask = 0;
  if (!Settings::DepthOfField())
    mask |= kPostFxDepthOfField | kPostFxBokeh;
  if (!Settings::MotionBlur())
    mask |= kPostFxMotionBlur | kPostFxCameraMotionBlur;
  if (!Settings::RadialBlur())
    mask |= kPostFxRadialBlur;
  if (!Settings::HeatEffects())
    mask |= kPostFxHeatVision | kPostFxHeatHaze;
  if (!Settings::ColorGrading())
    mask |= kPostFxColorization;
  if (!Settings::FilmGrain())
    mask |= kPostFxGrain;
  if (!Settings::EdgeFilter())
    mask |= kPostFxEdgeFilter;
  if (!Settings::Halo())
    mask |= kPostFxHalo;
  return mask;
}

std::atomic<uint32_t> g_last_flags{0xFFFFFFFFu};

double FovScale() {
  const double s = Settings::FovScale();
  return (s > 0.25 && s < 4.0) ? s : 1.0;
}

std::atomic<int> g_fov_logs{0};

}

REX_HOOK_RAW(eot_RenderComposition_ExecuteChain) {
  const uint32_t cmd = ctx.r4.u32;
  if (cmd) {
    const uint32_t flags = eot::mem::load<uint32_t>(cmd + kCommandFlags);
    const uint32_t bloom = eot::mem::load<uint8_t>(cmd + kCommandBloomEnable);
    const uint32_t seen = flags | (bloom ? 0x80000000u : 0u);
    if (g_last_flags.exchange(seen) != seen)
      EOT_INFO("[postfx] stages {:#06x} bloom {} (dof {} mblur {} radial {} heat {}/{} grade {} "
               "grain {} edge {} halo {} bokeh {})",
               flags & 0xFFFF, bloom, (flags >> 1) & 1, (flags >> 2) & 1, (flags >> 3) & 1,
               (flags >> 5) & 1, (flags >> 6) & 1, (flags >> 8) & 1, (flags >> 10) & 1,
               (flags >> 11) & 1, (flags >> 12) & 1, (flags >> 15) & 1);
    const uint32_t mask = DisabledStages();
    if (mask & flags)
      eot::mem::store<uint32_t>(cmd + kCommandFlags, flags & ~mask);
    if (!Settings::Bloom() && bloom)
      eot::mem::store<uint8_t>(cmd + kCommandBloomEnable, 0);
  }
  __imp__eot_RenderComposition_ExecuteChain(ctx, base);
}

REX_HOOK_RAW(eot_GLAPICamera_SetFOVAngle) {
  const double scale = FovScale();
  if (g_fov_logs.load(std::memory_order_relaxed) < 4) {
    g_fov_logs.fetch_add(1, std::memory_order_relaxed);
    EOT_INFO("[postfx] SetFOVAngle({:#x}, {:#x}) = {:.4f} (scale {:.3f})", ctx.r3.u32, ctx.r4.u32,
             ctx.f1.f64, scale);
  }
  if (scale != 1.0)
    ctx.f1.f64 *= scale;
  __imp__eot_GLAPICamera_SetFOVAngle(ctx, base);
}

REX_HOOK_RAW(eot_GLAPICamera_GetFOVAngle) {
  __imp__eot_GLAPICamera_GetFOVAngle(ctx, base);
  const double scale = FovScale();
  if (scale != 1.0)
    ctx.f1.f64 /= scale;
}
