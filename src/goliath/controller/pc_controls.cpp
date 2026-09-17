#include "goliath/controller/pc_controls.h"

#include <cstdint>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mnk_input_driver.h>

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

}

void InstallPcControls() {
  using namespace rex::input;
  std::vector<rex::input::mnk::KeyboardAction> actions = {
      Button("jump", "eot_key_jump", X_INPUT_GAMEPAD_A),
      Button("light_attack", "eot_key_light_attack", X_INPUT_GAMEPAD_X),
      Button("heavy_attack", "eot_key_heavy_attack", X_INPUT_GAMEPAD_Y),
      Button("web", "eot_key_web", X_INPUT_GAMEPAD_B),
      Button("grab", "eot_key_grab", X_INPUT_GAMEPAD_RIGHT_SHOULDER),
      Button("special_attack", "eot_key_special_attack", X_INPUT_GAMEPAD_LEFT_SHOULDER),
      Trigger("web_swing", "eot_key_web_swing", false),
      Trigger("hyper_sense", "eot_key_hyper_sense", true),
      Button("left_stick_click", "eot_key_left_stick_click", X_INPUT_GAMEPAD_LEFT_THUMB),
      Button("right_stick_click", "eot_key_right_stick_click", X_INPUT_GAMEPAD_RIGHT_THUMB),
      Button("spider_sense", "eot_key_spider_sense", X_INPUT_GAMEPAD_DPAD_UP),
      Button("upgrades", "eot_key_upgrades", X_INPUT_GAMEPAD_BACK),
      Button("pause", "eot_key_pause", X_INPUT_GAMEPAD_START),
      Button("dpad_down", "eot_key_dpad_down", X_INPUT_GAMEPAD_DPAD_DOWN),
      Button("dpad_left", "eot_key_dpad_left", X_INPUT_GAMEPAD_DPAD_LEFT),
      Button("dpad_right", "eot_key_dpad_right", X_INPUT_GAMEPAD_DPAD_RIGHT),
  };
  rex::input::mnk::SetActions(std::move(actions));
}

}
