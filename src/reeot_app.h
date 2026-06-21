// reeot - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include "goliath_engine/gpu/renderer/video.h"

class ReeotApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<ReeotApp>(new ReeotApp(ctx, "reeot",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.graphics = nullptr;
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}

  void OnPreLaunchModule() override {
    if (auto* w = window()) {
      eot::gpu::VideoInit(w->GetNativeWindowHandle(), 1280, 720);
    }
  }

  void OnShutdown() override { eot::gpu::VideoShutdown(); }
};
