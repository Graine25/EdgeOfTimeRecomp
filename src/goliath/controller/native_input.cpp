#include "goliath/controller/native_input.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/keybinds.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_Input_ReadPad);

REXCVAR_DEFINE_BOOL(eot_native_input, true, "EdgeOfTime/Input",
                    "Read the keyboard and mouse at the game's own pad poll, straight into the engine's "
                    "pad state, instead of through an emulated controller. Off puts the SDK's keyboard "
                    "driver back.");

namespace eot::controller {

namespace {

constexpr uint32_t kPadTables = 0x824C79F8;
constexpr uint32_t kPorts = 4;
constexpr uint32_t kSlots = 18;
enum Slot : uint32_t {
  kA = 0, kB, kX, kY, kLB, kRB, kLT, kRT, kLX, kLY, kRX, kRY, kStart, kBack, kL3, kR3, kDpadX, kDpadY
};

constexpr float kMousePerPixel = 200.0f / 32767.0f;

constexpr uint8_t kModShift = 1, kModCtrl = 2, kModAlt = 4;
struct Chord {
  uint8_t vk;
  uint8_t mods;
};

struct Action {
  const char *cvar;
  Slot slot;
  float value;
  Slot second;
  float second_value;
};
constexpr Action kActions[] = {
    {"eot_key_jump", kA, 1.0f, kA, 0.0f},
    {"eot_key_web", kB, 1.0f, kA, 0.0f},
    {"eot_key_light_attack", kX, 1.0f, kA, 0.0f},
    {"eot_key_heavy_attack", kY, 1.0f, kA, 0.0f},
    {"eot_key_special_attack", kLB, 1.0f, kA, 0.0f},
    {"eot_key_grab", kRB, 1.0f, kA, 0.0f},
    {"eot_key_hyper_sense", kLT, 1.0f, kA, 0.0f},
    {"eot_key_web_swing", kRT, 1.0f, kA, 0.0f},
    {"eot_key_pause", kStart, 1.0f, kA, 0.0f},
    {"eot_key_upgrades", kBack, 1.0f, kA, 0.0f},
    {"eot_key_time_stop", kL3, 1.0f, kR3, 1.0f},
    {"eot_key_center_camera", kR3, 1.0f, kA, 0.0f},
    {"eot_key_spider_sense", kDpadY, -1.0f, kA, 0.0f},
    {"eot_key_dpad_down", kDpadY, 1.0f, kA, 0.0f},
    {"eot_key_dpad_left", kDpadX, -1.0f, kA, 0.0f},
    {"eot_key_dpad_right", kDpadX, 1.0f, kA, 0.0f},
    {"eot_key_move_forward", kLY, -1.0f, kA, 0.0f},
    {"eot_key_move_back", kLY, 1.0f, kA, 0.0f},
    {"eot_key_move_left", kLX, -1.0f, kA, 0.0f},
    {"eot_key_move_right", kLX, 1.0f, kA, 0.0f},
};
constexpr size_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);

struct Bind {
  std::vector<Chord> chords;
};
std::mutex g_mutex;
Bind g_binds[kActionCount];
uint8_t g_bound_mods = 0;
bool g_binds_dirty = true;
bool g_callbacks = false;
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_mnk{false};
std::atomic<bool> g_mouse{true};
std::atomic<float> g_sensitivity{1.0f};
std::atomic<uint64_t> g_poll{0};
std::atomic<uint64_t> g_host_poll{0};
std::atomic<uint64_t> g_pad_poll{0};
bool g_announced = false;

std::string_view Trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ')
    s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ')
    s.remove_suffix(1);
  return s;
}

uint8_t ModifierOf(std::string_view name) {
  if (name == "Shift")
    return kModShift;
  if (name == "Ctrl" || name == "Control")
    return kModCtrl;
  if (name == "Alt")
    return kModAlt;
  return 0;
}

void Parse(std::string_view value, Bind &bind, uint8_t &bound_mods) {
  bind.chords.clear();
  while (!value.empty()) {
    const size_t comma = value.find(',');
    std::string_view token = Trim(value.substr(0, comma));
    value = comma == std::string_view::npos ? std::string_view() : value.substr(comma + 1);
    if (token.empty())
      continue;
    Chord chord{0, 0};
    while (true) {
      const size_t plus = token.find('+');
      if (plus == std::string_view::npos)
        break;
      chord.mods |= ModifierOf(Trim(token.substr(0, plus)));
      token = Trim(token.substr(plus + 1));
    }
    const rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(token);
    if (vk == rex::ui::VirtualKey::kNone)
      continue;
    chord.vk = static_cast<uint8_t>(vk);
    if (chord.mods == 0)
      bound_mods |= ModifierOf(token);
    bind.chords.push_back(chord);
  }
}

void ReadBinds() {
  uint8_t bound = 0;
  for (size_t i = 0; i < kActionCount; ++i)
    Parse(rex::cvar::GetFlagByName(kActions[i].cvar), g_binds[i], bound);
  g_bound_mods = bound;
  g_binds_dirty = false;
}

void ReadSettings() {
  g_enabled.store(rex::cvar::Query<bool>("eot_native_input"), std::memory_order_relaxed);
  g_mnk.store(rex::cvar::Query<bool>("mnk_mode"), std::memory_order_relaxed);
  g_mouse.store(rex::cvar::Query<bool>("mnk_mouse"), std::memory_order_relaxed);
  g_sensitivity.store(static_cast<float>(std::atof(rex::cvar::GetFlagByName("mnk_sensitivity").c_str())),
                      std::memory_order_relaxed);
  rex::input::mnk::SetGamepadEmulation(!(g_enabled.load() && g_mnk.load()));
}

void InstallCallbacks() {
  if (g_callbacks)
    return;
  g_callbacks = true;
  for (const Action &a : kActions)
    rex::cvar::RegisterChangeCallback(a.cvar, [](std::string_view, std::string_view) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_binds_dirty = true;
    });
  for (const char *name : {"eot_native_input", "mnk_mode", "mnk_mouse", "mnk_sensitivity"})
    rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) { ReadSettings(); });
  ReadSettings();
}

#if defined(_WIN32)
bool KeyDown(uint8_t vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
#else
bool KeyDown(uint8_t) { return false; }
#endif

uint8_t LiveModifiers() {
  uint8_t mods = 0;
  if (KeyDown(0x10))
    mods |= kModShift;
  if (KeyDown(0x11))
    mods |= kModCtrl;
  if (KeyDown(0x12))
    mods |= kModAlt;
  return mods;
}

bool Pressed(const Bind &bind, uint8_t live_mods) {
  for (const Chord &c : bind.chords) {
    if (c.mods != (live_mods & ~g_bound_mods))
      continue;
    if (KeyDown(c.vk))
      return true;
  }
  return false;
}

float LoadF(uint32_t at) { return std::bit_cast<float>(eot::mem::load<uint32_t>(at)); }
void StoreF(uint32_t at, float v) { eot::mem::store<uint32_t>(at, std::bit_cast<uint32_t>(v)); }

void Merge(uint32_t table, Slot slot, float value) {
  const uint32_t at = table + slot * 4;
  const float have = LoadF(at);
  if (slot >= kLX && slot <= kRY || slot >= kDpadX)
    StoreF(at, std::fabs(value) > std::fabs(have) ? value : have);
  else
    StoreF(at, std::max(have, value));
}

void FeedHost(uint32_t table) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_binds_dirty)
    ReadBinds();
  const uint8_t live = LiveModifiers();
  bool any = false;
  float lx = 0, ly = 0;
  for (size_t i = 0; i < kActionCount; ++i) {
    if (!Pressed(g_binds[i], live))
      continue;
    any = true;
    const Action &a = kActions[i];
    if (a.slot == kLX)
      lx += a.value;
    else if (a.slot == kLY)
      ly += a.value;
    else {
      Merge(table, a.slot, a.value);
      if (a.second_value != 0.0f)
        Merge(table, a.second, a.second_value);
    }
  }
  if (const float len = std::sqrt(lx * lx + ly * ly); len > 1.0f) {
    lx /= len;
    ly /= len;
  }
  if (lx != 0.0f)
    Merge(table, kLX, lx);
  if (ly != 0.0f)
    Merge(table, kLY, ly);

  float dx = 0, dy = 0;
  rex::input::mnk::DrainHostMouse(&dx, &dy);
  if (g_mouse.load(std::memory_order_relaxed) && (dx != 0.0f || dy != 0.0f)) {
    any = true;
    const float scale = kMousePerPixel * g_sensitivity.load(std::memory_order_relaxed);
    Merge(table, kRX, std::clamp(dx * scale, -1.0f, 1.0f));
    Merge(table, kRY, std::clamp(dy * scale, -1.0f, 1.0f));
  }
  if (any)
    g_host_poll.store(g_poll.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

bool AnyInput(uint32_t table) {
  for (uint32_t i = 0; i < kSlots; ++i)
    if (eot::mem::load<uint32_t>(table + i * 4) != 0)
      return true;
  return false;
}

}

bool NativeInputActive() { return g_enabled.load(std::memory_order_relaxed) && g_mnk.load(std::memory_order_relaxed); }
uint64_t LastHostInputPoll() { return g_host_poll.load(std::memory_order_relaxed); }
uint64_t LastPadInputPoll() { return g_pad_poll.load(std::memory_order_relaxed); }

}

REX_HOOK_RAW(eot_Input_ReadPad) {
  using namespace eot::controller;
  const uint32_t port = ctx.r3.u32;
  __imp__eot_Input_ReadPad(ctx, base);
  if (port != 0)
    return;
  InstallCallbacks();
  g_poll.fetch_add(1, std::memory_order_relaxed);
  const uint32_t ports = eot::mem::load<uint32_t>(kPadTables);
  const uint32_t table = ports && ports < 0xFFF00000u ? eot::mem::load<uint32_t>(ports + port * 4) : 0;
  if (!table || table >= 0xFFF00000u)
    return;
  if (AnyInput(table))
    g_pad_poll.store(g_poll.load(std::memory_order_relaxed), std::memory_order_relaxed);
  if (!NativeInputActive() || !rex::input::mnk::HostInputFocused())
    return;
  if (!g_announced) {
    g_announced = true;
    EOT_INFO("[input] keyboard and mouse read at the game's poll; the SDK's pad is idle");
  }
  FeedHost(table);
}
