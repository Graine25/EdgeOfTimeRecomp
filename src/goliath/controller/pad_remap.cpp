#include "goliath/controller/pad_remap.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <vector>
#include <rex/input/mnk/mnk_input_driver.h>
#include <cstdlib>
#include <string_view>
#include <rex/input/input_system.h>
#include <rex/runtime.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/controller/bind_capture.h"
#include "goliath/controller/menu_keys.h"

REX_EXTERN(__imp__eot_XInputGetState);

#define EOT_PAD "Input/Keybinds/Edge of Time (controller)"

REXCVAR_DEFINE_STRING(eot_pad_jump, "A", EOT_PAD, "Jump / Web Jump");
REXCVAR_DEFINE_STRING(eot_pad_web, "B", EOT_PAD, "Web Shot / Interact");
REXCVAR_DEFINE_STRING(eot_pad_light_attack, "X", EOT_PAD, "Melee attack button");
REXCVAR_DEFINE_STRING(eot_pad_heavy_attack, "Y", EOT_PAD, "Ranged Attack / Air Launcher");
REXCVAR_DEFINE_STRING(eot_pad_grab, "RB", EOT_PAD, "Grab or strike button");
REXCVAR_DEFINE_STRING(eot_pad_special_attack, "LB", EOT_PAD, "Throw object button");
REXCVAR_DEFINE_STRING(eot_pad_web_swing, "RT", EOT_PAD, "Web Zip / Web Swing");
REXCVAR_DEFINE_STRING(eot_pad_hyper_sense, "LT", EOT_PAD, "Special ability button");
REXCVAR_DEFINE_STRING(eot_pad_spider_sense, "Up", EOT_PAD, "Spider-Sense / Accelerated Vision");
REXCVAR_DEFINE_STRING(eot_pad_left_stick_click, "LS", EOT_PAD, "Stick to wall button");
REXCVAR_DEFINE_STRING(eot_pad_right_stick_click, "RS", EOT_PAD, "Center camera button");
REXCVAR_DEFINE_STRING(eot_pad_pause, "Start", EOT_PAD, "Pause menu button");
REXCVAR_DEFINE_STRING(eot_pad_upgrades, "Back", EOT_PAD, "Upgrades menu button");
REXCVAR_DEFINE_STRING(eot_pad_sticks, "normal", EOT_PAD, "Swap the sticks");

namespace eot::controller {

namespace {

constexpr uint8_t kTriggerPressed = 30;

constexpr const PadAction *kActions = kPadActions;

struct Table {
  std::array<PadInput, kPadActionCount> physical{};
  uint32_t claimed = 0;
  uint32_t native = 0;
  bool swapped = false;
  bool identity = true;
};
Table g_table;
std::mutex g_table_mutex;
std::atomic<bool> g_dirty{true};
uint32_t g_polls = 0;
constexpr uint32_t kRecheckPolls = 120;

constexpr uint32_t Bit(PadInput input) { return 1u << static_cast<uint32_t>(input); }

Table Build() {
  Table t;
  for (uint32_t i = 0; i < kPadActionCount; ++i) {
    const PadInput physical = PhysicalFor(kActions[i]);
    t.physical[i] = physical;
    t.claimed |= Bit(physical);
    t.native |= Bit(kActions[i].native);
    if (physical != kActions[i].native)
      t.identity = false;
  }
  t.swapped = SticksSwapped();
  if (t.swapped)
    t.identity = false;
  return t;
}

bool PressedIn(const RawPad &pad, PadInput input) {
  switch (input) {
  case PadInput::LT:
    return pad.left_trigger > kTriggerPressed;
  case PadInput::RT:
    return pad.right_trigger > kTriggerPressed;
  case PadInput::None:
    return false;
  default:
    return (pad.buttons & PadInputBit(input)) != 0;
  }
}

uint8_t TriggerFrom(const RawPad &pad, PadInput input) {
  switch (input) {
  case PadInput::LT:
    return pad.left_trigger;
  case PadInput::RT:
    return pad.right_trigger;
  default:
    return PressedIn(pad, input) ? 255 : 0;
  }
}

void Press(RawPad &out, PadInput native, const RawPad &in, PadInput physical) {
  switch (native) {
  case PadInput::LT:
    out.left_trigger = std::max(out.left_trigger, TriggerFrom(in, physical));
    break;
  case PadInput::RT:
    out.right_trigger = std::max(out.right_trigger, TriggerFrom(in, physical));
    break;
  default:
    if (PressedIn(in, physical))
      out.buttons |= PadInputBit(native);
    break;
  }
}

}

PadInput PhysicalFor(const PadAction &action) {
  const PadInput bound = ParsePadInput(rex::cvar::GetFlagByName(action.pad_cvar));
  return bound == PadInput::None ? action.native : bound;
}

bool SticksSwapped() { return rex::cvar::GetFlagByName(kPadSticksCvar) == "swapped"; }

void PadRemapChanged() { g_dirty.store(true, std::memory_order_release); }

bool PadRemapActive() {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  return !g_table.identity;
}

void RemapPad(RawPad &pad) {
  Table t;
  {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    t = g_table;
  }
  if (t.identity)
    return;
  RawPad out = pad;
  out.buttons = 0;
  out.left_trigger = 0;
  out.right_trigger = 0;
  for (uint32_t i = 0; i < kPadActionCount; ++i)
    Press(out, kActions[i].native, pad, t.physical[i]);
  for (const PadInputInfo &i : kPadInputs) {
    if (i.input == PadInput::None || (t.claimed & Bit(i.input)) || (t.native & Bit(i.input)))
      continue;
    if (i.input == PadInput::LT)
      out.left_trigger = std::max(out.left_trigger, pad.left_trigger);
    else if (i.input == PadInput::RT)
      out.right_trigger = std::max(out.right_trigger, pad.right_trigger);
    else
      out.buttons |= pad.buttons & i.bit;
  }
  out.buttons |= pad.buttons & rex::input::X_INPUT_GAMEPAD_GUIDE;
  if (t.swapped && ActivePad() != PadBrand::Keyboard) {
    std::swap(out.thumb_lx, out.thumb_rx);
    std::swap(out.thumb_ly, out.thumb_ry);
  }
  pad = out;
}

}

REX_HOOK_RAW(eot_XInputGetState) {
  using namespace eot::controller;
  const uint32_t state = ctx.r4.u32;
  __imp__eot_XInputGetState(ctx, base);
  if (ctx.r3.u32 != 0 || !state)
    return;
  if (g_dirty.exchange(false, std::memory_order_acq_rel) || ++g_polls % kRecheckPolls == 0) {
    Table t = Build();
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(g_table_mutex);
      changed = t.physical != g_table.physical || t.swapped != g_table.swapped;
      g_table = t;
    }
    if (changed) {
      eot::goliath::InstallPcControls();
      EOT_INFO("[pad] binds {}: {}{}", t.identity ? "native" : "remapped", [&] {
        std::string s;
        for (uint32_t i = 0; i < kPadActionCount; ++i)
          if (t.physical[i] != kActions[i].native)
            s += std::string(s.empty() ? "" : ", ") + kActions[i].id + " on " + PadInputName(t.physical[i]);
        return s.empty() ? std::string("every action on its own button") : s;
      }(), t.swapped ? "; sticks swapped" : "");
    }
  }
  RawPad pad;
  pad.buttons = eot::mem::load<uint16_t>(state + 4);
  pad.left_trigger = eot::mem::load<uint8_t>(state + 6);
  pad.right_trigger = eot::mem::load<uint8_t>(state + 7);
  pad.thumb_lx = eot::mem::load<int16_t>(state + 8);
  pad.thumb_ly = eot::mem::load<int16_t>(state + 10);
  pad.thumb_rx = eot::mem::load<int16_t>(state + 12);
  pad.thumb_ry = eot::mem::load<int16_t>(state + 14);
  const bool swallowed = FilterPadForCapture(pad);
  if (!swallowed) {
    RemapPad(pad);
    ApplyMenuKeys(pad);
  }
  eot::mem::store<uint16_t>(state + 4, pad.buttons);
  eot::mem::store<uint8_t>(state + 6, pad.left_trigger);
  eot::mem::store<uint8_t>(state + 7, pad.right_trigger);
  eot::mem::store<int16_t>(state + 8, pad.thumb_lx);
  eot::mem::store<int16_t>(state + 10, pad.thumb_ly);
  eot::mem::store<int16_t>(state + 12, pad.thumb_rx);
  eot::mem::store<int16_t>(state + 14, pad.thumb_ry);
}

#define EOT_KEYS "Input/Keybinds/Edge of Time"

REXCVAR_DEFINE_STRING(eot_key_jump, "Space", EOT_KEYS, "Jump key (A)");
REXCVAR_DEFINE_STRING(eot_key_light_attack, "LMB", EOT_KEYS, "Light attack key (X)");
REXCVAR_DEFINE_STRING(eot_key_heavy_attack, "MMB", EOT_KEYS, "Heavy attack key (Y)");
REXCVAR_DEFINE_STRING(eot_key_web, "E", EOT_KEYS, "Web / interact key (B)");
REXCVAR_DEFINE_STRING(eot_key_grab, "Q", EOT_KEYS, "Grab key (RB)");
REXCVAR_DEFINE_STRING(eot_key_special_attack, "V", EOT_KEYS, "Throw key (LB)");
REXCVAR_DEFINE_STRING(eot_key_web_swing, "RMB", EOT_KEYS, "Web swing key (RT)");
REXCVAR_DEFINE_STRING(eot_key_hyper_sense, "Shift", EOT_KEYS, "Hyper-Sense key (LT)");
REXCVAR_DEFINE_STRING(eot_key_left_stick_click, "Z", EOT_KEYS, "Left stick click key");
REXCVAR_DEFINE_STRING(eot_key_right_stick_click, "X", EOT_KEYS, "Right stick click key");
REXCVAR_DEFINE_STRING(eot_key_spider_sense, "R", EOT_KEYS, "Spider-Sense key (D-pad up)");
REXCVAR_DEFINE_STRING(eot_key_upgrades, "Tab", EOT_KEYS, "Upgrades key (Back)");
REXCVAR_DEFINE_STRING(eot_key_pause, "Escape", EOT_KEYS, "Pause key (Start)");
REXCVAR_DEFINE_STRING(eot_key_dpad_down, "C", EOT_KEYS, "D-pad down key");
REXCVAR_DEFINE_STRING(eot_key_dpad_left, "", EOT_KEYS, "D-pad left key");
REXCVAR_DEFINE_STRING(eot_key_dpad_right, "", EOT_KEYS, "D-pad right key");

namespace eot::goliath {

namespace {

constexpr uint16_t Buttons(int mask) { return static_cast<uint16_t>(mask); }

rex::input::mnk::KeyboardAction Button(const char *name, const char *cvar, int mask) {
  rex::input::mnk::KeyboardAction a;
  a.name = name;
  a.cvar = cvar;
  a.buttons = Buttons(mask);
  return a;
}

rex::input::mnk::KeyboardAction Trigger(const char *name, const char *cvar, bool left) {
  rex::input::mnk::KeyboardAction a;
  a.name = name;
  a.cvar = cvar;
  (left ? a.left_trigger : a.right_trigger) = 0xFF;
  return a;
}

rex::input::mnk::KeyboardAction On(const eot::controller::PadAction &action) {
  using eot::controller::PadInput;
  const PadInput physical = eot::controller::PhysicalFor(action);
  if (physical == PadInput::LT || physical == PadInput::RT)
    return Trigger(action.id, action.key_cvar, physical == PadInput::LT);
  return Button(action.id, action.key_cvar, eot::controller::PadInputBit(physical));
}

}

void InstallPcControls() {
  using namespace rex::input;
  std::vector<rex::input::mnk::KeyboardAction> actions;
  for (const eot::controller::PadAction &action : eot::controller::kPadActions)
    actions.push_back(On(action));
  actions.push_back(Button("dpad_down", "eot_key_dpad_down", X_INPUT_GAMEPAD_DPAD_DOWN));
  actions.push_back(Button("dpad_left", "eot_key_dpad_left", X_INPUT_GAMEPAD_DPAD_LEFT));
  actions.push_back(Button("dpad_right", "eot_key_dpad_right", X_INPUT_GAMEPAD_DPAD_RIGHT));
  rex::input::mnk::SetActions(std::move(actions));
}

}

namespace eot::controller {

namespace {

constexpr uint16_t kMicrosoft = 0x045E;
constexpr uint16_t kSony = 0x054C;
constexpr uint16_t kNintendo = 0x057E;
constexpr uint16_t kValve = 0x28DE;
constexpr uint16_t kSteamDeck = 0x1205;

uint16_t GuidWord(std::string_view guid, size_t byte) {
  if (guid.size() < (byte + 2) * 2)
    return 0;
  const std::string lo(guid.substr(byte * 2, 2));
  const std::string hi(guid.substr(byte * 2 + 2, 2));
  return static_cast<uint16_t>(std::strtoul(lo.c_str(), nullptr, 16) |
                               (std::strtoul(hi.c_str(), nullptr, 16) << 8));
}

bool Has(std::string_view name, std::string_view word) { return name.find(word) != std::string_view::npos; }

PadBrand BrandOf(const rex::input::DeviceInfo &info) {
  if (info.synthetic)
    return PadBrand::Keyboard;
  const uint16_t vendor = GuidWord(info.guid, 4);
  const uint16_t product = GuidWord(info.guid, 8);
  const std::string_view name = info.name;
  if (vendor == kValve)
    return product == kSteamDeck ? PadBrand::SteamDeck : PadBrand::Unknown;
  if (vendor == kSony || Has(name, "PS4") || Has(name, "PS5") || Has(name, "DualSense") || Has(name, "DualShock"))
    return PadBrand::PlayStation;
  if (vendor == kNintendo || Has(name, "Switch") || Has(name, "Joy-Con"))
    return PadBrand::Switch;
  if (Has(name, "Steam Deck"))
    return PadBrand::SteamDeck;
  if (vendor == kMicrosoft || Has(name, "Xbox"))
    return Has(name, "360") ? PadBrand::Xbox360 : PadBrand::XboxSeries;
  return PadBrand::Unknown;
}

}

const char *ToString(PadBrand brand) {
  switch (brand) {
  case PadBrand::Xbox360:
    return "xbox";
  case PadBrand::XboxSeries:
    return "xboxseries";
  case PadBrand::PlayStation:
    return "playstation";
  case PadBrand::Switch:
    return "switch";
  case PadBrand::SteamDeck:
    return "steamdeck";
  case PadBrand::Keyboard:
    return "keyboard";
  default:
    return "unknown";
  }
}

PadBrand ActivePad() {
  rex::Runtime *runtime = rex::Runtime::instance();
  if (!runtime || !runtime->input_system())
    return PadBrand::Unknown;
  auto *input = static_cast<rex::input::InputSystem *>(runtime->input_system());
  rex::input::DeviceInfo info;
  if (!input->ActiveDevice(0, &info))
    return PadBrand::Unknown;
  return BrandOf(info);
}

}
