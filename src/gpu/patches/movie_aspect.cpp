#include "gpu/patches/movie_aspect.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/patches/aspect_ratio.h"

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
