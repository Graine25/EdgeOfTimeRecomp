// gamelogic/patches/frame_rate.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gamelogic/ui/hud_api.h"

REX_EXTERN(__imp__eot_Key_UpdateTravel);
REX_EXTERN(__imp__eot_HiddenSpider_UpdateTravelToHero);
REX_EXTERN(__imp__eot_FXEmitterLight_UpdateActive);
REX_EXTERN(__imp__eot_EvadeClone_UpdateEvadeDone);

namespace {
using eot::mem::load;
using eot::mem::store;
using clock = std::chrono::steady_clock;

constexpr double kConsoleFrame = 1.0 / 60.0;

uint32_t NextState(uint32_t object) {
  const uint32_t record = load<uint8_t>(object + 18) == 1
                              ? object + 20
                              : load<uint32_t>(object + 20) + 16u * load<uint8_t>(object + 19);
  return load<uint16_t>(record + 2);
}

constexpr uint32_t kKeySpeed = 72;
constexpr uint32_t kKeySpeedCap = 76;
constexpr uint32_t kKeyAccel = 80;
constexpr uint32_t kKeyDelay = 84;
constexpr uint32_t kKeySlotHeld = 88;
constexpr uint32_t kContextualActionDone = 0x881B0060;

constexpr uint32_t kSpiderDelay = 48;
constexpr uint32_t kSpiderSpeed = 52;
constexpr float kSpiderAccel = 0.2f;
constexpr float kSpiderSpeedCap = 1.0f;

constexpr uint32_t kEngineFrameCount = 0x824E5A90;

template <typename T> struct Book {
  struct Entry {
    T value{};
    clock::time_point seen{};
  };
  std::mutex mutex;
  std::unordered_map<uint32_t, Entry> entries;
  clock::time_point pruned{};

  template <typename F> void With(uint32_t object, F &&f) {
    const auto now = clock::now();
    std::lock_guard lock(mutex);
    if (now - pruned > std::chrono::seconds(10)) {
      pruned = now;
      for (auto it = entries.begin(); it != entries.end();)
        it = now - it->second.seen > std::chrono::seconds(10) ? entries.erase(it) : std::next(it);
    }
    Entry &e = entries[object];
    f(e.value, e.seen == clock::time_point{} ? now : e.seen, now);
    e.seen = now;
  }
};

struct LightTick {
  double behind = 0.0;
};
Book<LightTick> g_lights;

struct CloneWait {
  uint32_t enter_frame = 0;
  uint32_t written = 0xFFFFFFFFu;
  clock::time_point entered{};
};
Book<CloneWait> g_clones;
}

REX_HOOK_RAW(eot_Key_UpdateTravel) {
  const uint32_t key = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const uint32_t next_before = NextState(key);
  const bool flying = load<float>(key + kKeyDelay) <= 0.0f && dt > 0.0;
  float speed_after = 0.0f, cap = 0.0f;
  if (flying) {
    const float accel = load<float>(key + kKeyAccel);
    const float k = static_cast<float>(dt * 60.0);
    cap = load<float>(key + kKeySpeedCap);
    speed_after = std::min(load<float>(key + kKeySpeed) + accel * static_cast<float>(dt), cap);
    store<float>(key + kKeySpeed, speed_after * k - accel * static_cast<float>(dt));
    store<float>(key + kKeySpeedCap, cap * k);
  }
  __imp__eot_Key_UpdateTravel(ctx, base);
  if (flying) {
    store<float>(key + kKeySpeedCap, cap);
    const float stepped = load<float>(key + kKeySpeed);
    const float full_step = speed_after * static_cast<float>(dt * 60.0);
    const bool arrived = NextState(key) != next_before || stepped < full_step * (1.0f - 1e-4f);
    if (!arrived)
      store<float>(key + kKeySpeed, speed_after);
  }
  if (NextState(key) != next_before && load<float>(key + kKeySlotHeld) > 0.0f) {
    store<float>(key + kKeySlotHeld, 0.0f);
    eot::ui::hud::CallAt(ctx, base, kContextualActionDone, 0);
    EOT_INFO("[keys] key {:#x} landed before giving the action button back; given back", key);
  }
}

REX_HOOK_RAW(eot_HiddenSpider_UpdateTravelToHero) {
  const uint32_t spider = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const float k = static_cast<float>(dt / kConsoleFrame);
  if (!(dt > 0.0) || k >= 1.0f || load<float>(spider + kSpiderDelay) > 0.0f) {
    __imp__eot_HiddenSpider_UpdateTravelToHero(ctx, base);
    return;
  }
  const float step_dt = static_cast<float>(dt);
  const float speed_after = std::min(load<float>(spider + kSpiderSpeed) + kSpiderAccel * step_dt, kSpiderSpeedCap);
  const uint32_t next_before = NextState(spider);
  store<float>(spider + kSpiderSpeed, speed_after * k - kSpiderAccel * step_dt);
  __imp__eot_HiddenSpider_UpdateTravelToHero(ctx, base);
  const bool arrived = NextState(spider) != next_before ||
                       load<float>(spider + kSpiderSpeed) < speed_after * k * (1.0f - 1e-4f);
  if (!arrived)
    store<float>(spider + kSpiderSpeed, speed_after);
}

REX_HOOK_RAW(eot_FXEmitterLight_UpdateActive) {
  const uint32_t light = ctx.r3.u32;
  bool tick = false;
  g_lights.With(light, [&](LightTick &t, clock::time_point last, clock::time_point now) {
    t.behind = std::clamp(t.behind + std::chrono::duration<double>(now - last).count(), -kConsoleFrame,
                          kConsoleFrame);
    if (last == now || t.behind + 0.002 >= kConsoleFrame) {
      tick = true;
      t.behind -= kConsoleFrame;
    }
  });
  if (tick)
    __imp__eot_FXEmitterLight_UpdateActive(ctx, base);
}

REX_HOOK_RAW(eot_EvadeClone_UpdateEvadeDone) {
  constexpr uint32_t kEnterFrame = 84;
  constexpr auto kWait = std::chrono::milliseconds(50);
  const uint32_t clone = ctx.r3.u32;
  const uint32_t frame = load<uint32_t>(kEngineFrameCount);
  const uint32_t stored = load<uint32_t>(clone + kEnterFrame);
  uint32_t write = stored;
  g_clones.With(clone, [&](CloneWait &c, clock::time_point, clock::time_point now) {
    if (stored != c.written) {
      c.enter_frame = stored;
      c.entered = now;
    }
    write = now - c.entered < kWait ? frame : c.enter_frame;
    c.written = write;
  });
  store<uint32_t>(clone + kEnterFrame, write);
  __imp__eot_EvadeClone_UpdateEvadeDone(ctx, base);
}
