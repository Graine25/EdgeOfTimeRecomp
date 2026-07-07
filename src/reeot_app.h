#pragma once

#include <rex/rex_app.h>

#include "core/logging.h"
#include "gpu/device/device.h"
#include "gpu/shaders/guest_shaders.h"

class ReeotApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<ReeotApp>(new ReeotApp(ctx, "reeot",
        PPCImageConfig));
  }

  void OnPostSetup() override {
    if (!eot::gpu::Video::CreateHostDevice()) {
      EOT_CRITICAL("Video::CreateHostDevice failed; quitting");
      app_context().QuitFromUIThread();
      return;
    }
    if (!eot::gpu::Video::CreateSwapChain(window())) {
      EOT_CRITICAL("Video::CreateSwapChain failed; quitting");
      app_context().QuitFromUIThread();
    }
  }

  void OnWindowPixelSizeChanged(uint32_t, uint32_t) override {
    eot::gpu::Video::RequestResize();
  }
  void OnShutdown() override { eot::gpu::LogShaderStats(); }
};
