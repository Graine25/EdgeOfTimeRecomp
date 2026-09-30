#include "goliath/controller/pc_controls.h"

#include <cstdint>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mnk_input_driver.h>

#include "goliath/controller/pad_remap.h"

#define EOT_KEYS "Input/Keybinds/Edge of Time"

REXCVAR_DEFINE_STRING(eot_key_jump, "Space", EOT_KEYS, "Jump (A)");
REXCVAR_DEFINE_STRING(eot_key_light_attack, "LMB", EOT_KEYS, "Light attack (X)");
REXCVAR_DEFINE_STRING(eot_key_heavy_attack, "MMB", EOT_KEYS, "Heavy attack (Y)");
REXCVAR_DEFINE_STRING(eot_key_web, "E", EOT_KEYS, "Web / interact (B)");
REXCVAR_DEFINE_STRING(eot_key_grab, "Q", EOT_KEYS, "Grab (RB)");
REXCVAR_DEFINE_STRING(eot_key_special_attack, "V", EOT_KEYS, "Special attack / throw object (LB)");
REXCVAR_DEFINE_STRING(eot_key_web_swing, "RMB", EOT_KEYS, "Web swing, hold (RT)");
REXCVAR_DEFINE_STRING(eot_key_hyper_sense, "Shift", EOT_KEYS,
                      "Hyper-Sense / Accelerated Decoy (LT)");
REXCVAR_DEFINE_STRING(eot_key_left_stick_click, "Z", EOT_KEYS, "Left stick click (L3); Time Stop with R3");
REXCVAR_DEFINE_STRING(eot_key_right_stick_click, "X", EOT_KEYS, "Right stick click (R3): center camera; Time Stop with L3");
REXCVAR_DEFINE_STRING(eot_key_time_paradox, "", EOT_KEYS, "Time Stop, on one key (L3 + R3)");
REXCVAR_DEFINE_STRING(eot_key_spider_sense, "R", EOT_KEYS, "Spider-Sense (D-pad up)");
REXCVAR_DEFINE_STRING(eot_key_upgrades, "Tab", EOT_KEYS, "Upgrades (Back)");
REXCVAR_DEFINE_STRING(eot_key_pause, "Escape", EOT_KEYS, "Pause (Start)");
REXCVAR_DEFINE_STRING(eot_key_dpad_down, "C", EOT_KEYS, "D-pad down");
REXCVAR_DEFINE_STRING(eot_key_dpad_left, "", EOT_KEYS, "D-pad left");
REXCVAR_DEFINE_STRING(eot_key_dpad_right, "", EOT_KEYS, "D-pad right");

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
