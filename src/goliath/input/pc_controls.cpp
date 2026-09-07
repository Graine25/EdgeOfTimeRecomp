#include "goliath/input/pc_controls.h"

#include <cstdint>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/mnk/mnk_input_driver.h>

#define EOT_KEYS "Input/Keybinds/Edge of Time"

REXCVAR_DEFINE_STRING(eot_key_jump, "Space", EOT_KEYS, "Jump (A)");
REXCVAR_DEFINE_STRING(eot_key_light_attack, "LMB", EOT_KEYS, "Light attack (X)");
REXCVAR_DEFINE_STRING(eot_key_heavy_attack, "E", EOT_KEYS, "Heavy attack (Y)");
REXCVAR_DEFINE_STRING(eot_key_web, "F", EOT_KEYS, "Web / interact (B)");
REXCVAR_DEFINE_STRING(eot_key_grab, "MMB", EOT_KEYS, "Grab (RB)");
REXCVAR_DEFINE_STRING(eot_key_web_swing, "RMB", EOT_KEYS, "Web swing, hold (RT)");
REXCVAR_DEFINE_STRING(eot_key_hyper_sense, "Shift", EOT_KEYS,
                      "Hyper-Sense / Accelerated Decoy (LT)");
REXCVAR_DEFINE_STRING(eot_key_time_stop, "Q", EOT_KEYS, "Time Stop (L3 + R3)");
REXCVAR_DEFINE_STRING(eot_key_spider_sense, "R", EOT_KEYS, "Spider-Sense (D-pad up)");
REXCVAR_DEFINE_STRING(eot_key_center_camera, "C", EOT_KEYS, "Center camera (R3)");
REXCVAR_DEFINE_STRING(eot_key_upgrades, "Tab", EOT_KEYS, "Upgrades (Back)");
REXCVAR_DEFINE_STRING(eot_key_pause, "Return", EOT_KEYS, "Pause (Start)");
REXCVAR_DEFINE_STRING(eot_key_dpad_down, "", EOT_KEYS, "D-pad down");
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

}

void InstallPcControls() {
  using namespace rex::input;
  std::vector<rex::input::mnk::KeyboardAction> actions = {
      Button("jump", "eot_key_jump", X_INPUT_GAMEPAD_A),
      Button("light_attack", "eot_key_light_attack", X_INPUT_GAMEPAD_X),
      Button("heavy_attack", "eot_key_heavy_attack", X_INPUT_GAMEPAD_Y),
      Button("web", "eot_key_web", X_INPUT_GAMEPAD_B),
      Button("grab", "eot_key_grab", X_INPUT_GAMEPAD_RIGHT_SHOULDER),
      Trigger("web_swing", "eot_key_web_swing", false),
      Trigger("hyper_sense", "eot_key_hyper_sense", true),
      Button("time_stop", "eot_key_time_stop",
             X_INPUT_GAMEPAD_LEFT_THUMB | X_INPUT_GAMEPAD_RIGHT_THUMB),
      Button("spider_sense", "eot_key_spider_sense", X_INPUT_GAMEPAD_DPAD_UP),
      Button("center_camera", "eot_key_center_camera", X_INPUT_GAMEPAD_RIGHT_THUMB),
      Button("upgrades", "eot_key_upgrades", X_INPUT_GAMEPAD_BACK),
      Button("pause", "eot_key_pause", X_INPUT_GAMEPAD_START),
      Button("dpad_down", "eot_key_dpad_down", X_INPUT_GAMEPAD_DPAD_DOWN),
      Button("dpad_left", "eot_key_dpad_left", X_INPUT_GAMEPAD_DPAD_LEFT),
      Button("dpad_right", "eot_key_dpad_right", X_INPUT_GAMEPAD_DPAD_RIGHT),
  };
  rex::input::mnk::SetActions(std::move(actions));
}

}
