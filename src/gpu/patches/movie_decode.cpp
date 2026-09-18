#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

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
