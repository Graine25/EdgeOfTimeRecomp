#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_UpdateGate_Tick);
REX_EXTERN(__imp__eot_GLAPILogic_SetUpdateFrequency);
REX_EXTERN(__imp__eot_GLAPIEmitter_SetUpdateFrequency);

REXCVAR_DEFINE_BOOL(eot_update_lod, false, "EdgeOfTime/Graphics", "Keep the console's update LOD");

namespace {

constexpr uint32_t kGateTick = 16;
constexpr uint32_t kGateSkip = 32;

std::atomic<uint64_t> g_gate_calls{0}, g_gate_forced{0};

std::mutex g_seen_mutex;
std::unordered_set<uint64_t> g_seen;
uint32_t g_seen_logged = 0;

void LogCurves(const char *kind, uint32_t object, uint32_t params) {
  uint64_t hash = 0x9E3779B97F4A7C15ull;
  for (uint32_t i = 0; i < 72; i += 4)
    hash = (hash ^ eot::mem::load<uint32_t>(params + i)) * 0x100000001B3ull;
  {
    std::lock_guard lock(g_seen_mutex);
    if (!g_seen.insert(hash).second || g_seen_logged >= 48)
      return;
    ++g_seen_logged;
  }
  auto f = [&](uint32_t off) { return eot::mem::load<float>(params + off); };
  EOT_INFO("[update-lod] {} {:#x}: in view ({:.0f}m {:.3f}s | {:.0f}m {:.3f}s | {:.0f}m {:.3f}s, cull "
           "{:.0f}m) hidden ({:.0f}m {:.3f}s | {:.0f}m {:.3f}s | {:.0f}m {:.3f}s, cull {:.0f}m) "
           "flags {:#x} {:#x} {:#x} {:#x}",
           kind, object, f(8), f(12), f(16), f(20), f(24), f(28), f(32), f(44), f(48), f(52),
           f(56), f(60), f(64), f(68), eot::mem::load<uint32_t>(params),
           eot::mem::load<uint32_t>(params + 4), eot::mem::load<uint32_t>(params + 36),
           eot::mem::load<uint32_t>(params + 40));
}

void LogSummary() {
  using clock = std::chrono::steady_clock;
  static clock::time_point last = clock::now();
  const auto now = clock::now();
  if (now - last < std::chrono::seconds(30))
    return;
  last = now;
  const uint64_t calls = g_gate_calls.exchange(0), forced = g_gate_forced.exchange(0);
  if (calls)
    EOT_INFO("[update-lod] last 30 s: {} gate calls, {} skips ticked instead ({:.1f}%)", calls,
             forced, 100.0 * static_cast<double>(forced) / static_cast<double>(calls));
}

}

REX_HOOK_RAW(eot_UpdateGate_Tick) {
  __imp__eot_UpdateGate_Tick(ctx, base);
  g_gate_calls.fetch_add(1, std::memory_order_relaxed);
  if (ctx.r3.u32 == kGateSkip && !REXCVAR_GET(eot_update_lod)) {
    ctx.r3.u32 = kGateTick;
    g_gate_forced.fetch_add(1, std::memory_order_relaxed);
  }
  LogSummary();
}

REX_HOOK_RAW(eot_GLAPILogic_SetUpdateFrequency) {
  const uint32_t object = ctx.r4.u32, params = ctx.r5.u32;
  __imp__eot_GLAPILogic_SetUpdateFrequency(ctx, base);
  if (params)
    LogCurves("logic", object, params);
}

REX_HOOK_RAW(eot_GLAPIEmitter_SetUpdateFrequency) {
  const uint32_t object = ctx.r4.u32, params = ctx.r5.u32;
  __imp__eot_GLAPIEmitter_SetUpdateFrequency(ctx, base);
  if (params)
    LogCurves("emitter", object, params);
}
