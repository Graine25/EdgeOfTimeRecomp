#include "reeot_app.h"

#include <functional>
#include <optional>
#include <string>

#include <rex/cvar.h>
#include <rex/runtime.h>

#include "generated/default/reeot_pch.h"

#include "core/build_info.h"
#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/pipeline/pipeline_cache.h"

REXCVAR_DECLARE(bool, mnk_mode);

std::unique_ptr<rex::ui::WindowedApp> ReeotApp::Create(rex::ui::WindowedAppContext &ctx) {
  return std::unique_ptr<ReeotApp>(new ReeotApp(ctx));
}

ReeotApp::ReeotApp(rex::ui::WindowedAppContext &ctx) : rex::ReXApp(ctx, "reeot", PPCImageConfig) {}

ReeotApp::~ReeotApp() = default;

REXCVAR_DEFINE_STRING(eot_data_root, "", "eot", "Game data folder");

std::optional<rex::PathConfig>
ReeotApp::OnFinalizePaths(const rex::PathConfig &defaults,
                          std::function<void(rex::PathConfig)> resume) {
  (void)resume;
  rex::PathConfig paths = defaults;
  const std::string root = REXCVAR_GET(eot_data_root);
  if (!root.empty())
    paths.game_data_root = root;
  return paths;
}

void ReeotApp::OnPreSetup(rex::RuntimeConfig &config) {
  REXCVAR_SET(mnk_mode, true);
  config.graphics = nullptr;
}

void ReeotApp::OnPreLaunchModule() {
  EOT_INFO("reeot {} ({}@{}{}) renderer starting", REEOT_VERSION_STRING, REEOT_GIT_BRANCH,
           REEOT_GIT_COMMIT, REEOT_GIT_DIRTY ? "+" : "");
  if (!eot::gpu::Video::CreateHostDevice(window())) {
    EOT_ERROR("Host device creation failed: the guest will run headless");
    return;
  }
  eot::gpu::GuestShadersInit();
  eot::gpu::PsoCachePrecache();
}

void ReeotApp::OnShutdown() {
  eot::gpu::Video::BeginShutdown();
  eot::gpu::Video::Shutdown();
}

void ReeotApp::OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) {
  (void)pixel_width;
  (void)pixel_height;
  eot::gpu::Video::RequestResize();
}

bool ReeotApp::OnWindowCloseRequested() {
  eot::gpu::Video::BeginShutdown();
  return true;
}
