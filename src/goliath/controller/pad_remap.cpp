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

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/controller/bind_capture.h"
#include "goliath/controller/menu_keys.h"
#include "goliath/controller/pad_identity.h"
#include "goliath/controller/pc_controls.h"
#include "goliath/debug/input_script.h"

REX_EXTERN(__imp__eot_XInputGetState);

#define EOT_PAD "Input/Keybinds/Edge of Time (controller)"

REXCVAR_DEFINE_STRING(eot_pad_jump, "A", EOT_PAD, "Jump / Web Jump");
REXCVAR_DEFINE_STRING(eot_pad_web, "B", EOT_PAD, "Web Shot / Interact");
REXCVAR_DEFINE_STRING(eot_pad_light_attack, "X", EOT_PAD, "Melee Attack");
REXCVAR_DEFINE_STRING(eot_pad_heavy_attack, "Y", EOT_PAD, "Ranged Attack / Air Launcher");
REXCVAR_DEFINE_STRING(eot_pad_grab, "RB", EOT_PAD, "Grab / Strike");
REXCVAR_DEFINE_STRING(eot_pad_special_attack, "LB", EOT_PAD, "Throw");
REXCVAR_DEFINE_STRING(eot_pad_web_swing, "RT", EOT_PAD, "Web Zip / Web Swing");
REXCVAR_DEFINE_STRING(eot_pad_hyper_sense, "LT", EOT_PAD, "Special Ability");
REXCVAR_DEFINE_STRING(eot_pad_spider_sense, "Up", EOT_PAD, "Spider-Sense / Accelerated Vision");
REXCVAR_DEFINE_STRING(eot_pad_left_stick_click, "LS", EOT_PAD, "Stick to a wall (left stick click)");
REXCVAR_DEFINE_STRING(eot_pad_right_stick_click, "RS", EOT_PAD, "Center the camera (right stick click)");
REXCVAR_DEFINE_STRING(eot_pad_pause, "Start", EOT_PAD, "Pause Menu");
REXCVAR_DEFINE_STRING(eot_pad_upgrades, "Back", EOT_PAD, "Upgrades Menu");
REXCVAR_DEFINE_STRING(eot_pad_sticks, "normal", EOT_PAD, "Sticks: normal or swapped");

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
  eot::debug::InputScriptPad(pad);
  eot::mem::store<uint16_t>(state + 4, pad.buttons);
  eot::mem::store<uint8_t>(state + 6, pad.left_trigger);
  eot::mem::store<uint8_t>(state + 7, pad.right_trigger);
  eot::mem::store<int16_t>(state + 8, pad.thumb_lx);
  eot::mem::store<int16_t>(state + 10, pad.thumb_ly);
  eot::mem::store<int16_t>(state + 12, pad.thumb_rx);
  eot::mem::store<int16_t>(state + 14, pad.thumb_ry);
}
