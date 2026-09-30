#include "gpu/patches/aspect_ratio.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>

#include <rex/hook.h>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <chrono>

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

REX_EXTERN(__imp__eot_Movie_DrawFrame);
REX_EXTERN(__imp__eot_Movie_FitRect);
REX_EXTERN(__imp__eot_MovieTarget_CreateForTexture); // (slot r3, texture record r4, flag r5)
REX_EXTERN(__imp__eot_RenderCommand_DrawMovie);

namespace {
std::atomic<bool> g_movie_drawn{false};

constexpr uint32_t kRectX0 = 68, kRectY0 = 72;
constexpr uint32_t kRectX1 = 84, kRectY1 = 88;
constexpr uint32_t kRectX2 = 100, kRectY2 = 104;

void StoreF32(uint32_t va, float v) { eot::mem::store<uint32_t>(va, std::bit_cast<uint32_t>(v)); }
float LoadF32(uint32_t va) { return std::bit_cast<float>(eot::mem::load<uint32_t>(va)); }

struct DrawnMovie {
  uint32_t width = 0, height = 0;
  float rect[4] = {};
};
std::unordered_map<uint32_t, DrawnMovie> g_last_drawn;

constexpr uint32_t kFrameReady = 118;
bool g_in_movie_command = false;
std::atomic<bool> g_skip_resolve{false};
uint32_t g_skipped = 0;
}

namespace eot::gpu {

bool TakeMovieDrawnFlag() { return g_movie_drawn.exchange(false, std::memory_order_acq_rel); }

bool TakeMovieResolveSkip() { return g_skip_resolve.exchange(false, std::memory_order_acq_rel); }

}

REX_HOOK_RAW(eot_RenderCommand_DrawMovie) {
  g_in_movie_command = true;
  __imp__eot_RenderCommand_DrawMovie(ctx, base);
  g_in_movie_command = false;
  g_skip_resolve.store(false, std::memory_order_release);
}

REX_HOOK_RAW(eot_Movie_DrawFrame) {
  const uint32_t state = ctx.r3.u32;
  if (g_in_movie_command && state && eot::mem::load<uint8_t>(state + kFrameReady) == 0) {
    g_skip_resolve.store(true, std::memory_order_release);
    if (g_skipped++ < 8)
      EOT_INFO("[movie] state {:#x}: no frame decoded yet; the menu texture is cleared instead of resolved", state);
  }
  if (state) {
    DrawnMovie now;
    now.width = eot::mem::load<uint32_t>(state + 4);
    now.height = eot::mem::load<uint32_t>(state + 8);
    now.rect[0] = LoadF32(state + kRectX0);
    now.rect[1] = LoadF32(state + kRectY0);
    now.rect[2] = LoadF32(state + kRectX2);
    now.rect[3] = LoadF32(state + kRectY1);
    auto [it, fresh] = g_last_drawn.try_emplace(state);
    DrawnMovie &last = it->second;
    if (fresh || last.width != now.width || last.height != now.height ||
        std::memcmp(last.rect, now.rect, sizeof(now.rect)) != 0) {
      last = now;
      EOT_INFO("[movie] state {:#x}: {}x{} frames drawn into x {:.3f}..{:.3f}, y {:.3f}..{:.3f} (clip space)", state,
               now.width, now.height, now.rect[0], now.rect[2], now.rect[3], now.rect[1]);
    }
  }
  __imp__eot_Movie_DrawFrame(ctx, base);
  g_movie_drawn.store(true, std::memory_order_release);
}

REX_HOOK_RAW(eot_MovieTarget_CreateForTexture) {
  const uint32_t slot = ctx.r3.u32, record = ctx.r4.u32;
  __imp__eot_MovieTarget_CreateForTexture(ctx, base);
  const uint32_t target = record ? eot::mem::load<uint32_t>(record + 88) : 0;
  const uint32_t resident = record ? eot::mem::load<uint32_t>(record + 92) : 0;
  if (!target)
    return;
  EOT_INFO("[movie] slot {} on texture {:#x}: its own image {}x{} (descriptor {:#x}, header {:#x}); the movie's "
           "target {}x{} (descriptor {:#x}, header {:#x})",
           slot, record, resident ? eot::mem::load<uint32_t>(resident + 12) : 0,
           resident ? eot::mem::load<uint32_t>(resident + 16) : 0, resident,
           resident ? eot::mem::load<uint32_t>(resident + 4) : 0, eot::mem::load<uint32_t>(target + 12),
           eot::mem::load<uint32_t>(target + 16), target, eot::mem::load<uint32_t>(target + 4));
}

REX_HOOK_RAW(eot_Movie_FitRect) {
  const uint32_t state = ctx.r3.u32;
  const float guessed = static_cast<float>(ctx.f1.f64);
  const float movie = static_cast<float>(ctx.f2.f64);
  if (!state || guessed <= 0.0f || movie <= 0.0f) {
    __imp__eot_Movie_FitRect(ctx, base);
    return;
  }
  const float display = std::clamp(eot::gpu::ConfiguredAspectRatio(), 0.5f, 4.5f);
  float half_w = 1.0f, half_h = 1.0f;
  if (display > movie)
    half_w = movie / display;
  else if (display < movie)
    half_h = display / movie;
  StoreF32(state + kRectX0, -half_w);
  StoreF32(state + kRectY0, half_h);
  StoreF32(state + kRectX1, -half_w);
  StoreF32(state + kRectY1, -half_h);
  StoreF32(state + kRectX2, half_w);
  StoreF32(state + kRectY2, half_h);
  EOT_INFO("[patch] movie {:.4f} on a {:.4f} display (the game guessed {:.4f}): rect {:.1f}% x {:.1f}%",
           movie, display, guessed, 100.0f * half_w, 100.0f * half_h);
}

REX_EXTERN(__imp__eot_CriMvEasyPlayer_SetUsableProcessors);
REX_EXTERN(__imp__eot_MovieTimer_GetTime);

namespace {

constexpr uint32_t kTimerState = 4;
constexpr uint32_t kStateRunning = 2;

constexpr double kTimeConstant = 0.33;
constexpr double kMaxLead = 0.04;

struct Smoother {
  uint32_t timer = 0;
  double offset = 0.0;
  double last_raw = -1.0;
  double last_out = 0.0;
  double last_host = 0.0;
  bool primed = false;
};
Smoother g_smooth;

double HostSeconds() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point t0 = clock::now();
  return std::chrono::duration<double>(clock::now() - t0).count();
}

}

REX_HOOK_RAW(eot_CriMvEasyPlayer_SetUsableProcessors) {
  const uint32_t params = ctx.r4.u32;
  for (uint32_t i = 0; i < 6; ++i)
    eot::mem::store<uint32_t>(params + i * 4, 0);
  __imp__eot_CriMvEasyPlayer_SetUsableProcessors(ctx, base);
}

REX_HOOK_RAW(eot_MovieTimer_GetTime) {
  const uint32_t self = ctx.r3.u32, pcount = ctx.r4.u32, punit = ctx.r5.u32;
  __imp__eot_MovieTimer_GetTime(ctx, base);
  const uint64_t unit = eot::mem::load<uint64_t>(punit);
  if (!unit || eot::mem::load<uint32_t>(self + kTimerState) != kStateRunning)
    return;
  const double raw = static_cast<double>(eot::mem::load<uint64_t>(pcount)) / static_cast<double>(unit);
  const double host = HostSeconds();
  Smoother &s = g_smooth;
  if (!s.primed || s.timer != self || raw + 0.5 < s.last_raw || raw > s.last_raw + 1.0) {
    s.timer = self;
    s.offset = raw - host;
    s.last_out = raw;
    s.primed = true;
  } else {
    const double alpha = 1.0 - std::exp(-(host - s.last_host) / kTimeConstant);
    s.offset += alpha * ((raw - host) - s.offset);
  }
  s.last_raw = raw;
  s.last_host = host;
  double out = host + s.offset;
  out = std::min(out, raw + kMaxLead);
  out = std::max(out, s.last_out);
  s.last_out = out;
  eot::mem::store<uint64_t>(pcount, static_cast<uint64_t>(out * static_cast<double>(unit) + 0.5));
}
