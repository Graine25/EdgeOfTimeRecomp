#include "goliath/controller/mouse_input.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string_view>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/runtime.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/ui/window_listener.h>

#include "core/logging.h"
#include "gpu/settings.h"

namespace eot::controller {

namespace {

constexpr size_t kZOrder = 24;

using clock = std::chrono::steady_clock;
constexpr auto kMenuBarFresh = std::chrono::milliseconds(150);
constexpr float kMeantMotionPixels = 32.0f;
constexpr auto kMotionWindow = std::chrono::milliseconds(500);
constexpr auto kCursorRest = std::chrono::seconds(3);

std::atomic<int64_t> g_menu_bar_ns{0};

class MouseInput final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
public:
  void Attach(rex::ui::Window *window) {
    if (!window || window_)
      return;
    window_ = window;
    window->AddInputListener(this, kZOrder);
    window->AddListener(this);
  }

  void Take(float *dx, float *dy) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool live = focused_ && fullscreen_.load(std::memory_order_relaxed) && !cursor_shown_;
    if (dx)
      *dx = live ? dx_ : 0.0f;
    if (dy)
      *dy = live ? dy_ : 0.0f;
    dx_ = dy_ = 0.0f;
  }

  void ApplyPolicy(bool fullscreen) {
    fullscreen_.store(fullscreen, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cursor_shown_ = false;
      meant_motion_ = 0.0f;
    }
    rex::input::mnk::SetMouseLookActive(fullscreen);
    if (window_)
      window_->SetCursorVisibility(fullscreen ? rex::ui::Window::CursorVisibility::kHidden
                                              : rex::ui::Window::CursorVisibility::kVisible);
    EOT_INFO("[input] {}: mouse look {}, cursor {}", fullscreen ? "fullscreen" : "windowed",
             fullscreen ? "on" : "off", fullscreen ? "hidden" : "visible");
  }

  void Tick(bool overlay_wants_pointer) {
    if (!window_ || !fullscreen_.load(std::memory_order_relaxed))
      return;
    const auto now = clock::now();
    const bool menu = now.time_since_epoch().count() - g_menu_bar_ns.load(std::memory_order_acquire) <=
                      std::chrono::duration_cast<clock::duration>(kMenuBarFresh).count();
    bool want = overlay_wants_pointer;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (menu && focused_) {
        if (cursor_shown_ ? now - last_motion_ <= kCursorRest : meant_motion_ >= kMeantMotionPixels)
          want = true;
      }
      if (!menu)
        meant_motion_ = 0.0f;
      changed = want != cursor_shown_;
      cursor_shown_ = want;
      if (changed)
        meant_motion_ = 0.0f;
    }
    if (changed) {
      rex::input::mnk::SetMouseLookActive(!want);
      EOT_INFO("[input] cursor {}: {}", want ? "shown" : "hidden",
               want ? (overlay_wants_pointer ? "an overlay takes input" : "the mouse moved on a menu")
                    : (menu ? "the mouse rested" : "no menu, the camera has the mouse"));
    }
    const auto visibility =
        want ? rex::ui::Window::CursorVisibility::kVisible : rex::ui::Window::CursorVisibility::kHidden;
    if (changed || (want && window_->GetCursorVisibility() != visibility))
      window_->SetCursorVisibility(visibility);
  }

  void OnMouseMove(rex::ui::MouseEvent &e) override {
    std::lock_guard<std::mutex> lock(mutex_);
    float mx = 0.0f, my = 0.0f;
    if (e.dx() != 0.0f || e.dy() != 0.0f) {
      mx = e.dx();
      my = e.dy();
    } else if (have_position_) {
      mx = static_cast<float>(e.x() - x_);
      my = static_cast<float>(e.y() - y_);
    }
    dx_ += mx;
    dy_ += my;
    x_ = e.x();
    y_ = e.y();
    have_position_ = true;
    if (mx != 0.0f || my != 0.0f) {
      const auto now = clock::now();
      if (now - last_motion_ > kMotionWindow)
        meant_motion_ = 0.0f;
      meant_motion_ += std::fabs(mx) + std::fabs(my);
      last_motion_ = now;
    }
  }

  void OnLostFocus(rex::ui::UISetupEvent &) override {
    std::lock_guard<std::mutex> lock(mutex_);
    focused_ = false;
    dx_ = dy_ = 0.0f;
    meant_motion_ = 0.0f;
    have_position_ = false;
  }
  void OnGotFocus(rex::ui::UISetupEvent &) override {
    std::lock_guard<std::mutex> lock(mutex_);
    focused_ = true;
    have_position_ = false;
  }
  void OnClosing(rex::ui::UIEvent &) override {
    if (window_) {
      window_->RemoveInputListener(this);
      window_->RemoveListener(this);
      window_ = nullptr;
    }
  }

private:
  rex::ui::Window *window_ = nullptr;
  std::atomic<bool> fullscreen_{true};
  std::mutex mutex_;
  float dx_ = 0.0f, dy_ = 0.0f;
  int32_t x_ = 0, y_ = 0;
  bool have_position_ = false;
  bool focused_ = true;
  bool cursor_shown_ = false;
  float meant_motion_ = 0.0f;
  clock::time_point last_motion_{};
};

MouseInput g_mouse;

}

void AttachMouseInput(rex::ui::Window *window) {
  g_mouse.Attach(window);
  g_mouse.ApplyPolicy(eot::gpu::Settings::Fullscreen());
  rex::cvar::RegisterChangeCallback("fullscreen", [](std::string_view, std::string_view value) {
    const bool fullscreen = value == "true" || value == "1";
    rex::Runtime *runtime = rex::Runtime::instance();
    if (runtime && runtime->app_context())
      runtime->app_context()->CallInUIThread([fullscreen] { g_mouse.ApplyPolicy(fullscreen); });
    else
      g_mouse.ApplyPolicy(fullscreen);
  });
}

void MouseCursorTick(bool overlay_wants_pointer) { g_mouse.Tick(overlay_wants_pointer); }

void NoteMenuBarShown() {
  g_menu_bar_ns.store(clock::now().time_since_epoch().count(), std::memory_order_release);
}

}

extern "C" __declspec(dllexport) void eot_mouse_take(float *dx, float *dy) { eot::controller::g_mouse.Take(dx, dy); }
