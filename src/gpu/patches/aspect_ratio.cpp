#include "gpu/patches/aspect_ratio.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/settings.h"

REX_EXTERN(__imp__eot_GfxDevice_SetupPresentParams);

namespace {

constexpr uint32_t kCameraAspectRatio = 0x824E4E10;
constexpr uint32_t kDisplayAspectRatio = 0x82496C08;
constexpr uint32_t kAspectClass = 0x82496C10;

bool Plausible(float ratio) { return std::isfinite(ratio) && ratio >= 0.5f && ratio <= 4.5f; }

uint32_t AspectClassOf(float ratio) {
  if (ratio <= 0.0f)
    return 4;
  if (ratio <= 1.25f)
    return 0;
  if (ratio <= 1.333f)
    return 1;
  if (ratio <= 1.6f)
    return 2;
  return 3;
}

float PresetRatio(const std::string &preset) {
  if (preset == "4:3")
    return 4.0f / 3.0f;
  if (preset == "16:10")
    return 16.0f / 10.0f;
  if (preset == "21:9")
    return 21.0f / 9.0f;
  if (preset == "32:9")
    return 32.0f / 9.0f;
  if (preset == "auto") {
    const uint32_t w = eot::gpu::Video::OutputWidth(), h = eot::gpu::Video::OutputHeight();
    if (w && h) {
      const float ratio = static_cast<float>(w) / static_cast<float>(h);
      if (Plausible(ratio))
        return ratio;
    }
  }
  return 16.0f / 9.0f;
}

std::atomic<float> g_ratio{16.0f / 9.0f};
std::atomic<float> g_camera_ratio{16.0f / 9.0f};

constexpr float kWidescreenThreshold = 1.6f;

std::atomic<int> g_table_holds{0};

float CameraRatioFor(float ratio) {
  return std::fabs(ratio - kWidescreenThreshold) < 0.01f ? std::nextafter(kWidescreenThreshold, 0.0f)
                                                        : ratio;
}

}

namespace eot::gpu {

float ConfiguredAspectRatio() { return g_ratio.load(std::memory_order_relaxed); }

bool LayoutIsWidescreen() {
  return g_camera_ratio.load(std::memory_order_relaxed) >= kWidescreenThreshold;
}

bool MacWide() {
  return std::fabs(g_ratio.load(std::memory_order_relaxed) - kWidescreenThreshold) < 0.01f &&
         g_camera_ratio.load(std::memory_order_relaxed) < kWidescreenThreshold;
}

CameraRatioHold::CameraRatioHold(float value) : saved_(eot::mem::load<uint32_t>(kCameraAspectRatio)) {
  g_table_holds.fetch_add(1, std::memory_order_acq_rel);
  eot::mem::store<uint32_t>(kCameraAspectRatio, std::bit_cast<uint32_t>(value));
}

CameraRatioHold::~CameraRatioHold() {
  eot::mem::store<uint32_t>(kCameraAspectRatio, saved_);
  g_table_holds.fetch_sub(1, std::memory_order_acq_rel);
}

void ApplyAspectRatio() {
  const float ratio = PresetRatio(Settings::EffectiveAspectRatio());
  if (!Plausible(ratio))
    return;
  g_ratio.store(ratio, std::memory_order_relaxed);
  const float camera = CameraRatioFor(ratio);
  g_camera_ratio.store(camera, std::memory_order_relaxed);

  const uint32_t bits = std::bit_cast<uint32_t>(camera);
  if (g_table_holds.load(std::memory_order_acquire) > 0)
    return;
  const uint32_t live = eot::mem::load<uint32_t>(kCameraAspectRatio);
  if (live == bits)
    return;

  eot::mem::store<uint32_t>(kCameraAspectRatio, bits);
  eot::mem::store<uint32_t>(kDisplayAspectRatio, bits);
  eot::mem::store<uint32_t>(kAspectClass, AspectClassOf(camera));

  static int corrections = 0;
  if (corrections < 8) {
    ++corrections;
    EOT_INFO("[patch] aspect ratio {} = {:.4f} (camera {:.7f}, {} layout, class {}); guest held {:.4f}",
             Settings::EffectiveAspectRatio(), ratio, camera,
             camera >= kWidescreenThreshold ? "wide" : "4:3", AspectClassOf(camera),
             std::bit_cast<float>(live));
  }
}

}

REX_HOOK_RAW(eot_GfxDevice_SetupPresentParams) {
  __imp__eot_GfxDevice_SetupPresentParams(ctx, base);
  eot::gpu::ApplyAspectRatio();
}
