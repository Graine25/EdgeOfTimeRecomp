#include "gpu/patches/movie_aspect.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/patches/aspect_ratio.h"

REX_EXTERN(__imp__eot_Movie_DrawFrame);
REX_EXTERN(__imp__eot_Movie_FitRect);

namespace {
std::atomic<bool> g_movie_drawn{false};

constexpr uint32_t kRectX0 = 68, kRectY0 = 72;
constexpr uint32_t kRectX1 = 84, kRectY1 = 88;
constexpr uint32_t kRectX2 = 100, kRectY2 = 104;

void StoreF32(uint32_t va, float v) { eot::mem::store<uint32_t>(va, std::bit_cast<uint32_t>(v)); }
}

namespace eot::gpu {

bool TakeMovieDrawnFlag() { return g_movie_drawn.exchange(false, std::memory_order_acq_rel); }

}

REX_HOOK_RAW(eot_Movie_DrawFrame) {
  __imp__eot_Movie_DrawFrame(ctx, base);
  g_movie_drawn.store(true, std::memory_order_release);
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
