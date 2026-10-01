#include "goliath/controller/menu_keys.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/controller/button_glyphs.h"
#include "goliath/controller/mouse_input.h"
#include "goliath/controller/pad_identity.h"

namespace eot::controller {

namespace {

using clock = std::chrono::steady_clock;

constexpr size_t kZOrder = 25;

REXCVAR_DEFINE_DOUBLE(eot_wheel_camera_turn, 60.0, "EdgeOfTime/Input",
                      "Away from a menu, a notch of the wheel turns the camera by this many "
                      "counts of sideways mouse motion at sensitivity 1 -- about six degrees, "
                      "and the same six whatever the sensitivity slider is set to. Raise it for "
                      "a coarser nudge; zero leaves the wheel alone outside menus.");

constexpr auto kMashFresh = std::chrono::milliseconds(200);

constexpr uint32_t kActionSlots = 0x883C92F8;
constexpr uint32_t kActionSlotBusy = 0x883C930C;
constexpr uint32_t kActionSlotCount = 5;

constexpr uint32_t kPressHold = 1;
constexpr uint32_t kPressGap = 2;
constexpr uint32_t kWheelHold = 3;
constexpr int32_t kWheelMax = 4;

std::atomic<bool> g_escape{false};
std::atomic<bool> g_delete{false};
std::atomic<bool> g_backspace{false};
std::atomic<bool> g_space{false};
std::atomic<bool> g_left_button{false};
std::atomic<uint32_t> g_press_a{0};
std::atomic<uint32_t> g_press_b{0};
std::atomic<uint32_t> g_press_x{0};
std::atomic<int32_t> g_wheel{0};
std::atomic<int64_t> g_mash_ns{0};
std::atomic<uint16_t> g_mash_buttons{0};

struct Pulse {
  uint32_t taken = 0;
  uint32_t left = 0;
  uint32_t gap = 0;

  bool Step(std::atomic<uint32_t> &presses) {
    if (left) {
      if (--left == 0)
        gap = kPressGap;
      return true;
    }
    if (gap) {
      --gap;
      return false;
    }
    if (presses.load(std::memory_order_acquire) == taken)
      return false;
    ++taken;
    left = kPressHold - 1;
    return true;
  }

  void Drop(const std::atomic<uint32_t> &presses) {
    taken = presses.load(std::memory_order_acquire);
    left = gap = 0;
  }
};
Pulse g_pulse_a, g_pulse_b, g_pulse_x;

std::atomic<uint16_t> g_from_left_button{0};
std::atomic<uint16_t> g_from_escape{0};
std::atomic<uint16_t> g_from_delete{0};
std::atomic<uint16_t> g_from_space{0};
clock::time_point g_keys_read{};
constexpr auto kKeysInterval = std::chrono::milliseconds(500);

std::string FirstKey(const char *cvar) {
  std::string value = rex::cvar::GetFlagByName(cvar);
  if (const size_t comma = value.find(','); comma != std::string::npos)
    value.resize(comma);
  if (const size_t plus = value.rfind('+'); plus != std::string::npos)
    value.erase(0, plus + 1);
  while (!value.empty() && value.back() == ' ')
    value.pop_back();
  while (!value.empty() && value.front() == ' ')
    value.erase(0, 1);
  return value;
}

uint16_t ButtonsFor(std::string_view key) {
  uint16_t buttons = 0;
  for (const PadAction &action : kPadActions)
    if (FirstKey(action.key_cvar) == key)
      buttons |= PadInputBit(PhysicalFor(action));
  return buttons;
}

void RefreshKeyButtons() {
  const clock::time_point now = clock::now();
  if (g_keys_read != clock::time_point{} && now - g_keys_read < kKeysInterval)
    return;
  g_keys_read = now;
  g_from_left_button.store(ButtonsFor("LMB"), std::memory_order_relaxed);
  g_from_escape.store(ButtonsFor("Escape"), std::memory_order_relaxed);
  g_from_delete.store(static_cast<uint16_t>(ButtonsFor("Delete") | ButtonsFor("Backspace")),
                      std::memory_order_relaxed);
  g_from_space.store(ButtonsFor("Space"), std::memory_order_relaxed);
}

bool KeyboardInHand() { return ActivePad() == PadBrand::Keyboard; }

bool InMenu() { return KeyboardInHand() && BarShowsPrompts(); }

bool WheelInMenu() { return BarShowsPrompts(); }

bool ContextualActionOnOffer() {
  static bool mapped = false;
  if (!mapped) {
    if (!eot::mem::readable(kActionSlots, 4 * kActionSlotCount) ||
        !eot::mem::readable(kActionSlotBusy, kActionSlotCount))
      return false;
    mapped = true;
  }
  for (uint32_t slot = 0; slot < kActionSlotCount; ++slot)
    if (eot::mem::load<uint32_t>(kActionSlots + 4 * slot) != 0xFFFFFFFFu)
      return eot::mem::load<uint8_t>(kActionSlotBusy + slot) == 0;
  return false;
}

bool g_space_down = false;
bool g_space_acts = false;

bool MashFresh() {
  const int64_t at = g_mash_ns.load(std::memory_order_acquire);
  return at != 0 && clock::now().time_since_epoch().count() - at <=
                        std::chrono::duration_cast<clock::duration>(kMashFresh).count();
}

class MenuKeys final : public rex::ui::WindowInputListener {
public:
  void Attach(rex::ui::Window *window) {
    if (!window || attached_)
      return;
    attached_ = true;
    window->AddInputListener(this, kZOrder);
  }

  void OnKeyDown(rex::ui::KeyEvent &e) override { Note(e.virtual_key(), true); }
  void OnKeyUp(rex::ui::KeyEvent &e) override { Note(e.virtual_key(), false); }

  void OnMouseDown(rex::ui::MouseEvent &e) override {
    if (e.button() != rex::ui::MouseEvent::Button::kLeft)
      return;
    g_left_button.store(true, std::memory_order_release);
    g_press_a.fetch_add(1, std::memory_order_acq_rel);
  }
  void OnMouseUp(rex::ui::MouseEvent &e) override {
    if (e.button() == rex::ui::MouseEvent::Button::kLeft)
      g_left_button.store(false, std::memory_order_release);
  }

  void OnMouseWheel(rex::ui::MouseEvent &e) override {
    const int32_t notches = e.scroll_y() / static_cast<int32_t>(rex::ui::MouseEvent::kScrollPerDetent);
    if (!notches)
      return;
    int32_t waiting = g_wheel.load(std::memory_order_relaxed);
    int32_t wanted = (waiting != 0 && (waiting > 0) != (notches > 0)) ? notches : waiting + notches;
    wanted = wanted > kWheelMax ? kWheelMax : (wanted < -kWheelMax ? -kWheelMax : wanted);
    g_wheel.store(wanted, std::memory_order_release);
  }

private:
  static void Note(rex::ui::VirtualKey vk, bool down) {
    switch (vk) {
    case rex::ui::VirtualKey::kEscape:
      if (down && !g_escape.exchange(true, std::memory_order_acq_rel))
        g_press_b.fetch_add(1, std::memory_order_acq_rel);
      else if (!down)
        g_escape.store(false, std::memory_order_release);
      break;
    case rex::ui::VirtualKey::kDelete:
      if (down && !g_delete.exchange(true, std::memory_order_acq_rel))
        g_press_x.fetch_add(1, std::memory_order_acq_rel);
      else if (!down)
        g_delete.store(false, std::memory_order_release);
      break;
    case rex::ui::VirtualKey::kBack:
      if (down && !g_backspace.exchange(true, std::memory_order_acq_rel))
        g_press_x.fetch_add(1, std::memory_order_acq_rel);
      else if (!down)
        g_backspace.store(false, std::memory_order_release);
      break;
    case rex::ui::VirtualKey::kSpace:
      g_space.store(down, std::memory_order_release);
      break;
    default:
      break;
    }
  }

  bool attached_ = false;
};
MenuKeys g_listener;

int32_t g_pulse_dir = 0;
uint32_t g_pulse_left = 0;
uint32_t g_gap_left = 0;

}

void AttachMenuKeys(rex::ui::Window *window) { g_listener.Attach(window); }

bool MenuKeysActive() { return InMenu(); }

void NoteMashPrompt(uint16_t buttons) {
  const uint16_t was = g_mash_buttons.exchange(buttons, std::memory_order_acq_rel);
  g_mash_ns.store(clock::now().time_since_epoch().count(), std::memory_order_release);
  if (buttons != was)
    EOT_INFO("[input] a mash QTE counts pad buttons {:#06x}: the space bar presses them too", buttons);
}

void ApplyWheel(RawPad &pad) {
  if (!WheelInMenu()) {
    const int32_t waiting = g_wheel.exchange(0, std::memory_order_acq_rel);
    const double counts = waiting ? REXCVAR_GET(eot_wheel_camera_turn) : 0.0;
    if (counts > 0.0) {
      const double sens = std::atof(rex::cvar::GetFlagByName("mnk_sensitivity").c_str());
      MouseAddTurn(static_cast<float>(-waiting * counts / (sens > 0.05 ? sens : 1.0)));
    }
    g_pulse_dir = 0;
    g_pulse_left = g_gap_left = 0;
    return;
  }

  if (g_pulse_left) {
    --g_pulse_left;
    pad.buttons |= PadInputBit(g_pulse_dir > 0 ? PadInput::Right : PadInput::Left);
    if (!g_pulse_left)
      g_gap_left = kPressGap;
    return;
  }
  if (g_gap_left) {
    --g_gap_left;
    return;
  }
  const int32_t waiting = g_wheel.load(std::memory_order_acquire);
  if (!waiting)
    return;
  g_pulse_dir = waiting > 0 ? -1 : 1;
  g_wheel.store(waiting > 0 ? waiting - 1 : waiting + 1, std::memory_order_release);
  g_pulse_left = kWheelHold - 1;
  pad.buttons |= PadInputBit(g_pulse_dir > 0 ? PadInput::Right : PadInput::Left);
}

void ApplyMenuKeys(RawPad &pad) {
  if (rex::kernel::xam::xeXamInputBlocked()) {
    g_pulse_a.Drop(g_press_a);
    g_pulse_b.Drop(g_press_b);
    g_pulse_x.Drop(g_press_x);
    g_wheel.store(0, std::memory_order_release);
    g_pulse_dir = 0;
    g_pulse_left = g_gap_left = 0;
    g_space_acts = false;
    g_space_down = g_space.load(std::memory_order_acquire);
    return;
  }

  if (KeyboardInHand() && MashFresh() && g_space.load(std::memory_order_acquire))
    pad.buttons |= g_mash_buttons.load(std::memory_order_acquire);

  const bool space = KeyboardInHand() && g_space.load(std::memory_order_acquire);
  if (space && !g_space_down) {
    g_space_acts = !InMenu() && ContextualActionOnOffer();
    if (g_space_acts) {
      RefreshKeyButtons();
      static bool told = false;
      if (!told) {
        told = true;
        EOT_INFO("[input] a contextual action is on offer: the space bar answers it as B");
      }
    }
  }
  if (!space)
    g_space_acts = false;
  g_space_down = space;
  if (g_space_acts) {
    pad.buttons = static_cast<uint16_t>(pad.buttons & ~g_from_space.load(std::memory_order_relaxed));
    pad.buttons |= PadInputBit(PadInput::B);
  }

  ApplyWheel(pad);

  if (!InMenu()) {
    g_pulse_a.Drop(g_press_a);
    g_pulse_b.Drop(g_press_b);
    g_pulse_x.Drop(g_press_x);
    return;
  }
  RefreshKeyButtons();

  if (g_left_button.load(std::memory_order_acquire))
    pad.buttons = static_cast<uint16_t>(pad.buttons & ~g_from_left_button.load(std::memory_order_relaxed));
  if (g_escape.load(std::memory_order_acquire))
    pad.buttons = static_cast<uint16_t>(pad.buttons & ~g_from_escape.load(std::memory_order_relaxed));
  if (g_delete.load(std::memory_order_acquire) || g_backspace.load(std::memory_order_acquire))
    pad.buttons = static_cast<uint16_t>(pad.buttons & ~g_from_delete.load(std::memory_order_relaxed));
  if (g_pulse_a.Step(g_press_a))
    pad.buttons |= PadInputBit(PadInput::A);
  if (g_pulse_b.Step(g_press_b))
    pad.buttons |= PadInputBit(PadInput::B);
  if (g_pulse_x.Step(g_press_x))
    pad.buttons |= PadInputBit(PadInput::X);
}

}
