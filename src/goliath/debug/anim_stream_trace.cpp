#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>

#include <rex/hook.h>

#include "core/logging.h"

REX_EXTERN(__imp__eot_RZHAnim_LoadDiscardableData); // (animation r3, chunk r4, stream r5)

namespace {
using Clock = std::chrono::steady_clock;

constexpr auto kBurstGap = std::chrono::milliseconds(500);

struct Burst {
  std::mutex lock;
  Clock::time_point first{};
  Clock::time_point last{};
  uint32_t count = 0;
  int64_t slowest_us = 0;
};
Burst g_burst;

void Flush(Burst &burst) {
  if (!burst.count)
    return;
  const auto span = std::chrono::duration_cast<std::chrono::milliseconds>(burst.last - burst.first);
  EOT_INFO("[anim] {} animation(s) loaded over {} ms, ending {} ms ago (slowest {} us)", burst.count, span.count(),
           std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - burst.last).count(),
           burst.slowest_us);
  burst.count = 0;
  burst.slowest_us = 0;
}
}

REX_HOOK_RAW(eot_RZHAnim_LoadDiscardableData) {
  const auto start = Clock::now();
  __imp__eot_RZHAnim_LoadDiscardableData(ctx, base);
  const auto end = Clock::now();
  std::lock_guard<std::mutex> guard(g_burst.lock);
  if (g_burst.count && start - g_burst.last > kBurstGap)
    Flush(g_burst);
  if (!g_burst.count)
    g_burst.first = start;
  g_burst.last = end;
  ++g_burst.count;
  g_burst.slowest_us =
      std::max<int64_t>(g_burst.slowest_us, std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}
