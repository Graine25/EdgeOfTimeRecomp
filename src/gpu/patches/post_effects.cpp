#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/settings.h"

REX_EXTERN(__imp__eot_RenderComposition_ExecuteChain);
REX_EXTERN(__imp__eot_GLAPICamera_SetFOVAngle);
REX_EXTERN(__imp__eot_GLAPICamera_GetFOVAngle);
REX_EXTERN(__imp__sub_821AB250);
REX_EXTERN(__imp__eot_GLAPICamera_ShadowMapSetParams);

namespace {

using eot::gpu::Settings;

constexpr uint32_t kPostFxDepthOfField = 1u << 1;
constexpr uint32_t kPostFxMotionBlur = 1u << 2;
constexpr uint32_t kPostFxRadialBlur = 1u << 3;
constexpr uint32_t kPostFxHeatVision = 1u << 5;
constexpr uint32_t kPostFxHeatHaze = 1u << 6;
constexpr uint32_t kPostFxColorization = 1u << 8;
constexpr uint32_t kPostFxGrain = 1u << 10;
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

constexpr uint32_t kQualityLevel = 0x824E56D8;
std::atomic<int> g_quality_logs{0};

constexpr uint32_t kShadowCascades = 12;
constexpr uint32_t kShadowDistance = 16;
constexpr uint32_t kShadowAutoSplit = 56;
std::atomic<int> g_shadow_logs{0};

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
    const bool bloom_off = !Settings::Bloom() && bloom;
    if (mask & flags)
      eot::mem::store<uint32_t>(cmd + kCommandFlags, flags & ~mask);
    if (bloom_off)
      eot::mem::store<uint8_t>(cmd + kCommandBloomEnable, 0);
    const uint32_t applied = (flags & ~mask) | (bloom_off || !bloom ? 0u : 0x80000000u);
    static std::atomic<uint32_t> last_applied{0xFFFFFFFFu};
    if (((mask & flags) || bloom_off) && last_applied.exchange(applied) != applied)
      EOT_INFO("[postfx] applied {:#06x} bloom {} (requested {:#06x} bloom {}; options cleared {:#06x}{})",
               applied & 0xFFFF, bloom_off ? 0u : bloom, flags & 0xFFFF, bloom, mask & flags,
               bloom_off ? " + bloom byte" : "");
  }
  __imp__eot_RenderComposition_ExecuteChain(ctx, base);
}

REX_HOOK_RAW(sub_821AB250) {
  const uint32_t record = ctx.r3.u32;
  const int32_t forced = Settings::QualityLevel();
  if (forced >= 0 && eot::mem::load<uint32_t>(kQualityLevel) != static_cast<uint32_t>(forced)) {
    eot::mem::store<uint32_t>(kQualityLevel, static_cast<uint32_t>(forced));
    eot::mem::store<uint32_t>(kQualityLevel + 4, static_cast<uint32_t>(forced));
  }
  const uint32_t before = eot::mem::load<uint32_t>(kQualityLevel);
  __imp__sub_821AB250(ctx, base);
  if (!record || g_quality_logs.load(std::memory_order_relaxed) >= 24)
    return;
  const uint32_t count = eot::mem::load<uint32_t>(record + 496);
  if (count == 0 || count > 8)
    return;
  static uint32_t seen[24][10];
  uint32_t key[10] = {count, before};
  for (uint32_t i = 0; i < count; ++i)
    key[2 + i] = eot::mem::load<uint32_t>(record + 24 + 4 * i);
  const int n = g_quality_logs.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i)
    if (std::memcmp(seen[i], key, sizeof(key)) == 0)
      return;
  std::memcpy(seen[n], key, sizeof(key));
  g_quality_logs.fetch_add(1, std::memory_order_relaxed);
  std::string levels;
  for (uint32_t i = 0; i < count; ++i)
    levels += std::to_string(key[2 + i]) + (i + 1 < count ? "," : "");
  EOT_INFO("[quality] record {:#x}: levels [{}] quality {} -> {} (selected index {} mask {:#x})",
           record, levels, before, eot::mem::load<uint32_t>(kQualityLevel),
           eot::mem::load<uint32_t>(record + 500), eot::mem::load<uint32_t>(record + 152));
}

REX_HOOK_RAW(eot_GLAPICamera_ShadowMapSetParams) {
  const uint32_t params = ctx.r5.u32;
  if (params) {
    if (g_shadow_logs.load(std::memory_order_relaxed) < 6) {
      g_shadow_logs.fetch_add(1, std::memory_order_relaxed);
      std::string dump;
      for (uint32_t i = 0; i < 18; ++i) {
        const uint32_t v = eot::mem::load<uint32_t>(params + 4 * i);
        const float f = std::bit_cast<float>(v);
        if (v < 64u)
          dump += std::to_string(v);
        else
          dump += std::to_string(f);
        dump += i + 1 < 18 ? " " : "";
      }
      EOT_INFO("[shadow] ShadowMapSetParams({:#x}, {:#x}) block: {}", ctx.r3.u32, ctx.r4.u32, dump);
    }
    const int32_t cascades = Settings::ShadowCascades();
    const uint32_t had = eot::mem::load<uint32_t>(params + kShadowCascades);
    if (cascades >= 1 && cascades <= 4 && static_cast<uint32_t>(cascades) != had) {
      eot::mem::store<uint32_t>(params + kShadowCascades, static_cast<uint32_t>(cascades));
      bool auto_split = false;
      for (uint32_t i = had; i < static_cast<uint32_t>(cascades); ++i) {
        const float start = std::bit_cast<float>(eot::mem::load<uint32_t>(params + 24 + 8 * i));
        const float end = std::bit_cast<float>(eot::mem::load<uint32_t>(params + 28 + 8 * i));
        if (!(end > start))
          auto_split = true;
      }
      if (auto_split)
        eot::mem::store<uint8_t>(params + kShadowAutoSplit, 1);
      EOT_INFO("[shadow] cascades {} -> {}{}", had, cascades, auto_split ? " (engine split)" : "");
    }
    const double scale = Settings::ShadowDistanceScale();
    if (scale > 0.1 && scale < 10.0 && scale != 1.0) {
      static constexpr uint32_t kScaled[] = {0, kShadowDistance, 24, 28, 32, 36, 40, 44, 48, 52};
      for (uint32_t off : kScaled) {
        const float v = std::bit_cast<float>(eot::mem::load<uint32_t>(params + off));
        if (v > 0.0f)
          eot::mem::store<uint32_t>(params + off,
                                    std::bit_cast<uint32_t>(static_cast<float>(v * scale)));
      }
    }
  }
  __imp__eot_GLAPICamera_ShadowMapSetParams(ctx, base);
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
