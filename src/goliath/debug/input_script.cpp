#include "goliath/debug/input_script.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "core/logging.h"
#include "goliath/controller/pad_actions.h"

REXCVAR_DEFINE_STRING(
    eot_debug_script, "", "EdgeOfTime/Debug",
    "Pad input for the debug tools to put in, a step at a time. Steps are separated by spaces "
    "or semicolons: <action>[+<action>...][:frames] holds those actions, wait:<frames> holds "
    "nothing, stick:<x>,<y>[:frames] and rstick:<x>,<y>[:frames] push a stick (-1 to 1). "
    "Frames default to 2. An action is one of the game's own (jump, light_attack, web_swing, "
    "...) or a raw pad input (A, RT, Up). Setting this arms the script; it spends a frame only "
    "when the scene is actually moving, so under eot_debug_pause it waits for the step key. "
    "Example: wait:4 light_attack:2 wait:24 light_attack:2");

namespace {

using eot::controller::PadInput;
using eot::controller::RawPad;

constexpr size_t kMaxSteps = 64;
constexpr uint32_t kMaxFrames = 3600;
constexpr uint32_t kDefaultFrames = 2;
constexpr int16_t kStickFull = 32767;
constexpr uint8_t kTriggerFull = 255;

struct Step {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint32_t frames = kDefaultFrames;
  std::string label;
};

std::mutex g_mutex;
std::vector<Step> g_steps;
size_t g_at = 0;
uint32_t g_left = 0;
bool g_running = false;
Step g_live;
bool g_live_valid = false;

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return s;
}

bool ParseUint(std::string_view s, uint32_t &out) {
  s = Trim(s);
  if (s.empty())
    return false;
  uint32_t value = 0;
  const auto end = s.data() + s.size();
  const auto result = std::from_chars(s.data(), end, value);
  if (result.ec != std::errc() || result.ptr != end)
    return false;
  out = value;
  return true;
}

bool ParseAxis(std::string_view s, int16_t &out) {
  s = Trim(s);
  if (s.empty())
    return false;
  const std::string text(s);
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (!end || *end != '\0')
    return false;
  out = static_cast<int16_t>(std::clamp(value, -1.0, 1.0) * kStickFull);
  return true;
}

PadInput InputNamed(std::string_view name) {
  name = Trim(name);
  for (const eot::controller::PadAction &action : eot::controller::kPadActions)
    if (name == action.id)
      return action.native;
  return eot::controller::ParsePadInput(name);
}

void PressInto(Step &step, PadInput input) {
  switch (input) {
  case PadInput::LT:
    step.left_trigger = kTriggerFull;
    break;
  case PadInput::RT:
    step.right_trigger = kTriggerFull;
    break;
  default:
    step.buttons |= eot::controller::PadInputBit(input);
    break;
  }
}

bool ParseStep(std::string_view token, Step &step) {
  const size_t colon = token.find(':');
  const std::string_view head = Trim(token.substr(0, colon));
  std::string_view rest = colon == std::string_view::npos ? std::string_view{} : token.substr(colon + 1);

  if (head == "wait") {
    step.frames = kDefaultFrames;
    if (!rest.empty() && !ParseUint(rest, step.frames))
      return false;
    step.label = "wait";
    return true;
  }

  if (head == "stick" || head == "rstick") {
    const size_t frames_at = rest.find(':');
    std::string_view pair = rest.substr(0, frames_at);
    if (frames_at != std::string_view::npos && !ParseUint(rest.substr(frames_at + 1), step.frames))
      return false;
    const size_t comma = pair.find(',');
    if (comma == std::string_view::npos)
      return false;
    int16_t x = 0, y = 0;
    if (!ParseAxis(pair.substr(0, comma), x) || !ParseAxis(pair.substr(comma + 1), y))
      return false;
    if (head == "stick") {
      step.lx = x;
      step.ly = y;
    } else {
      step.rx = x;
      step.ry = y;
    }
    step.label = std::string(head);
    return true;
  }

  if (!rest.empty() && !ParseUint(rest, step.frames))
    return false;
  std::string_view names = head;
  bool any = false;
  while (!names.empty()) {
    const size_t plus = names.find('+');
    const std::string_view one = Trim(names.substr(0, plus));
    const PadInput input = InputNamed(one);
    if (input == PadInput::None)
      return false;
    PressInto(step, input);
    step.label += std::string(step.label.empty() ? "" : "+") + std::string(one);
    any = true;
    if (plus == std::string_view::npos)
      break;
    names.remove_prefix(plus + 1);
  }
  return any;
}

void Stop() {
  g_running = false;
  g_live_valid = false;
  g_at = 0;
  g_left = 0;
}

}

namespace eot::debug {

uint32_t InputScriptSet(std::string_view script) {
  std::vector<Step> steps;
  std::string_view rest = script;
  while (!rest.empty()) {
    const size_t at = rest.find_first_of(" \t;");
    const std::string_view token = Trim(rest.substr(0, at));
    rest = at == std::string_view::npos ? std::string_view{} : rest.substr(at + 1);
    if (token.empty())
      continue;
    if (steps.size() >= kMaxSteps) {
      EOT_WARN("[script] more than {} steps; the rest is ignored", kMaxSteps);
      break;
    }
    Step step;
    if (!ParseStep(token, step)) {
      EOT_WARN("[script] cannot read '{}'; skipped", std::string(token));
      continue;
    }
    step.frames = std::clamp(step.frames, 1u, kMaxFrames);
    steps.push_back(std::move(step));
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  Stop();
  g_steps = std::move(steps);
  if (g_steps.empty()) {
    if (!Trim(script).empty())
      EOT_WARN("[script] nothing to run");
    return 0;
  }
  g_running = true;
  uint32_t frames = 0;
  std::string listing;
  for (const Step &step : g_steps) {
    frames += step.frames;
    listing += std::string(listing.empty() ? "" : " ") + step.label + ":" + std::to_string(step.frames);
  }
  EOT_INFO("[script] {} step(s), {} frame(s): {}", g_steps.size(), frames, listing);
  return static_cast<uint32_t>(g_steps.size());
}

void InputScriptReplay() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_steps.empty()) {
    EOT_INFO("[script] nothing queued (set eot_debug_script)");
    return;
  }
  g_at = 0;
  g_left = 0;
  g_live_valid = false;
  g_running = true;
  EOT_INFO("[script] armed, {} step(s)", g_steps.size());
}

void InputScriptTick(bool simulating) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_live_valid = false;
  if (!g_running || !simulating)
    return;

  if (g_left == 0) {
    if (g_at >= g_steps.size()) {
      EOT_INFO("[script] done");
      Stop();
      return;
    }
    g_left = g_steps[g_at].frames;
  }

  g_live = g_steps[g_at];
  g_live_valid = true;
  if (--g_left == 0)
    ++g_at;
}

bool InputScriptPad(controller::RawPad &pad) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_live_valid)
    return false;
  pad.buttons = g_live.buttons;
  pad.left_trigger = g_live.left_trigger;
  pad.right_trigger = g_live.right_trigger;
  pad.thumb_lx = g_live.lx;
  pad.thumb_ly = g_live.ly;
  pad.thumb_rx = g_live.rx;
  pad.thumb_ry = g_live.ry;
  return true;
}

bool InputScriptHoldsInput() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_live_valid;
}

std::string InputScriptReadout() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_running || g_steps.empty())
    return {};
  if (g_left)
    return std::format("script  step {}/{}  {}  {} frame(s) left", g_at + 1, g_steps.size(),
                       g_steps[g_at].label, g_left);
  if (g_at < g_steps.size())
    return std::format("script  step {}/{} next  {}  {} frame(s)", g_at + 1, g_steps.size(),
                       g_steps[g_at].label, g_steps[g_at].frames);
  return std::format("script  {} step(s) done", g_steps.size());
}

}
