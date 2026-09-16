#include "goliath/controller/mouse_input.h"

#include <atomic>
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
    const bool live = focused_ && fullscreen_.load(std::memory_order_relaxed);
    if (dx)
      *dx = live ? dx_ : 0.0f;
    if (dy)
      *dy = live ? dy_ : 0.0f;
    dx_ = dy_ = 0.0f;
  }

  void ApplyPolicy(bool fullscreen) {
    fullscreen_.store(fullscreen, std::memory_order_relaxed);
    rex::input::mnk::SetMouseLookActive(fullscreen);
    if (window_)
      window_->SetCursorVisibility(fullscreen ? rex::ui::Window::CursorVisibility::kHidden
                                              : rex::ui::Window::CursorVisibility::kVisible);
    EOT_INFO("[input] {}: mouse look {}, cursor {}", fullscreen ? "fullscreen" : "windowed",
             fullscreen ? "on" : "off", fullscreen ? "hidden" : "visible");
  }

  void OnMouseMove(rex::ui::MouseEvent &e) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (e.dx() != 0.0f || e.dy() != 0.0f) {
      dx_ += e.dx();
      dy_ += e.dy();
    } else if (have_position_) {
      dx_ += static_cast<float>(e.x() - x_);
      dy_ += static_cast<float>(e.y() - y_);
    }
    x_ = e.x();
    y_ = e.y();
    have_position_ = true;
  }

  void OnLostFocus(rex::ui::UISetupEvent &) override {
    std::lock_guard<std::mutex> lock(mutex_);
    focused_ = false;
    dx_ = dy_ = 0.0f;
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

}

extern "C" __declspec(dllexport) void eot_mouse_take(float *dx, float *dy) { eot::controller::g_mouse.Take(dx, dy); }
