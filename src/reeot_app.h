#pragma once

#include <rex/rex_app.h>

#include "core/logging.h"
#include "gpu/device/device.h"

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
    }
  }
};
