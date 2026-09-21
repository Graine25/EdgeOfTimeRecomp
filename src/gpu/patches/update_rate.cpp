#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <algorithm>
#include <bit>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/ui/name_crc.h"
#include "goliath/debug/freecam.h"
#include "gpu/settings.h"

REX_EXTERN(__imp__eot_UpdateGate_Tick);
REX_EXTERN(__imp__eot_GLAPILogic_SetUpdateFrequency);
REX_EXTERN(__imp__eot_GLAPIEmitter_SetUpdateFrequency);
REX_EXTERN(__imp__eot_3dObj_AnimateSkeleton); // (object r3, seconds f1)

REXCVAR_DEFINE_BOOL(eot_update_lod, false, "EdgeOfTime/Graphics", "Keep the console's update LOD");

REXCVAR_DEFINE_STRING(eot_anim_fps, "", "EdgeOfTime/Graphics", "Fixed animation rate per rig");

namespace {

constexpr uint32_t kGateTick = 16;
constexpr uint32_t kGateSkip = 32;

constexpr uint32_t kObjectFlags = 300;
constexpr uint32_t kObjectDetached = 4;
constexpr uint32_t kObjectRenderData = 336;
constexpr uint32_t kRenderDataHierarchy = 44;
constexpr uint32_t kResourceCrc = 4;

constexpr const char *kHeroRig = "SpiderManHierarchy";
constexpr const char *kHeroAliases[] = {"hero", "spiderman", "spider-man", "miguel", "peter"};

struct RigStep {
  uint32_t crc;
  float interval;
};
std::mutex g_rigs_mutex;
std::vector<RigStep> g_rigs;
std::atomic<bool> g_rigs_any{false};
std::atomic<bool> g_rigs_stale{true};
std::atomic<bool> g_rigs_callback{false};

struct Held {
  float seconds = 0.0f;
  std::chrono::steady_clock::time_point seen;
};
std::mutex g_held_mutex;
std::unordered_map<uint32_t, Held> g_held;
std::atomic<uint64_t> g_steps_held{0}, g_steps_made{0};

uint32_t RigCrc(const std::string &name) {
  std::string lower;
  for (char c : name)
    lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
  for (const char *alias : kHeroAliases)
    if (lower == alias)
      return eot::ui::NameCrc(kHeroRig);
  const size_t digits = lower.compare(0, 2, "0x") == 0 ? 2 : 0;
  if (lower.size() == digits + 8 && lower.find_first_not_of("0123456789abcdef", digits) == std::string::npos)
    return static_cast<uint32_t>(std::strtoul(lower.c_str() + digits, nullptr, 16));
  return eot::ui::NameCrc(name.c_str());
}

void ParseRigs() {
  std::string text = REXCVAR_GET(eot_anim_fps);
  std::vector<RigStep> rigs;
  std::string entry;
  auto flush = [&]() {
    const size_t eq = entry.find_first_of("=:");
    if (eq != std::string::npos) {
      const std::string name = entry.substr(0, eq);
      const float fps = static_cast<float>(std::atof(entry.c_str() + eq + 1));
      if (!name.empty() && fps > 0.0f) {
        rigs.push_back({RigCrc(name), 1.0f / fps});
        EOT_INFO("[anim-step] rig {} ({:08x}) steps at {:g} fps", name, rigs.back().crc, fps);
      } else {
        EOT_WARN("[anim-step] ignoring '{}': expected Rig=fps", entry);
      }
    } else if (!entry.empty()) {
      EOT_WARN("[anim-step] ignoring '{}': expected Rig=fps", entry);
    }
    entry.clear();
  };
  for (char c : text) {
    if (c == ',' || c == ';' || c == ' ')
      flush();
    else
      entry.push_back(c);
  }
  flush();
  std::lock_guard lock(g_rigs_mutex);
  g_rigs = std::move(rigs);
  g_rigs_any.store(!g_rigs.empty(), std::memory_order_release);
}

float RigInterval(uint32_t crc) {
  std::lock_guard lock(g_rigs_mutex);
  for (const RigStep &r : g_rigs)
    if (r.crc == crc)
      return r.interval;
  return 0.0f;
}

void RefreshRigs() {
  if (!g_rigs_callback.exchange(true)) {
    rex::cvar::RegisterChangeCallback("eot_anim_fps", [](std::string_view, std::string_view) {
      g_rigs_stale.store(true, std::memory_order_release);
    });
  }
  if (g_rigs_stale.exchange(false))
    ParseRigs();
}

float ObjectInterval(uint32_t object) {
  if (eot::mem::load<uint32_t>(object + kObjectFlags) & kObjectDetached)
    return 0.0f;
  const uint32_t data = eot::mem::load<uint32_t>(object + kObjectRenderData);
  const uint32_t hierarchy = data ? eot::mem::load<uint32_t>(data + kRenderDataHierarchy) : 0;
  return hierarchy ? RigInterval(eot::mem::load<uint32_t>(hierarchy + kResourceCrc)) : 0.0f;
}

float HoldOrRelease(uint32_t object, float seconds, float interval) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_held_mutex);
  Held &held = g_held[object];
  held.seconds += seconds;
  held.seen = now;
  if (held.seconds + 1e-4f < interval)
    return 0.0f;
  const float sum = held.seconds;
  held.seconds = 0.0f;
  return sum;
}

void ForgetStaleHeld() {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_held_mutex);
  for (auto it = g_held.begin(); it != g_held.end();)
    it = now - it->second.seen > std::chrono::seconds(5) ? g_held.erase(it) : std::next(it);
}

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
  const uint64_t held = g_steps_held.exchange(0), made = g_steps_made.exchange(0);
  if (held || made)
    EOT_INFO("[anim-step] last 30 s: {} skeleton advances held, {} made", held, made);
  ForgetStaleHeld();
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

REX_HOOK_RAW(eot_3dObj_AnimateSkeleton) {
  RefreshRigs();
  if (g_rigs_any.load(std::memory_order_acquire)) {
    const uint32_t object = ctx.r3.u32;
    const float interval = ObjectInterval(object);
    if (interval > 0.0f) {
      const float sum = HoldOrRelease(object, static_cast<float>(ctx.f1.f64), interval);
      if (sum <= 0.0f) {
        g_steps_held.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      g_steps_made.fetch_add(1, std::memory_order_relaxed);
      ctx.f1.f64 = sum;
    }
  }
  __imp__eot_3dObj_AnimateSkeleton(ctx, base);
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

REX_EXTERN(__imp__eot_GEEngineMgrBC_UpdateFrameTime);

namespace {

constexpr uint32_t kFixedFrameTimeFlag = 0x824E5A83;
constexpr uint32_t kFrameDeltaSeconds = 0x824E5A8C;

constexpr double kMinDelta = 1.0 / 1000.0;
constexpr double kMaxDelta = 0.1;

bool UnlockWanted() {
  const int32_t fps = eot::gpu::Settings::FpsLimit();
  return fps == 0 || fps > 60;
}

}

REX_HOOK_RAW(eot_GEEngineMgrBC_UpdateFrameTime) {
  eot::debug::ScenePauseTick();
  eot::debug::FreecamTick();

  if (!UnlockWanted() || eot::mem::load<uint8_t>(kFixedFrameTimeFlag) != 0) {
    __imp__eot_GEEngineMgrBC_UpdateFrameTime(ctx, base);
    return;
  }

  using clock = std::chrono::steady_clock;
  static clock::time_point last{};
  const clock::time_point now = clock::now();
  double delta = (last.time_since_epoch().count() != 0)
                     ? std::chrono::duration<double>(now - last).count()
                     : 1.0 / 60.0;
  last = now;
  delta = std::clamp(delta, kMinDelta, kMaxDelta);

  eot::mem::store<uint32_t>(kFrameDeltaSeconds,
                            std::bit_cast<uint32_t>(static_cast<float>(delta)));
  ctx.f1.f64 = delta;
}

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

void PhysicsLogSummary() {
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
  PhysicsLogSummary();
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
