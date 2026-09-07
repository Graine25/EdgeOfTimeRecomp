#include "reeot_app.h"

#include <functional>
#include <optional>
#include <string>

#include <rex/cvar.h>
#include <rex/perf/counter.h>

#include <rex/runtime.h>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#endif

#include "generated/default/reeot_pch.h"

#include "core/build_info.h"
#include "core/logging.h"
#include "core/quit.h"
#include "gpu/device.h"
#include "gpu/settings.h"
#include "gpu/imgui_overlay.h"
#include "goliath/input/pc_controls.h"
#include "goliath/ui/overlays/fps.h"
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
  eot::goliath::InstallPcControls();
  if (eot::gpu::Settings::Profiler()) {
    rex::perf::Profiler::Startup();
    if (rex::perf::Profiler::is_enabled())
      EOT_INFO("Tracy profiler started; connect a viewer to capture.");
    else
      EOT_WARN("eot_profiler is set, but this build has no profiler compiled in.");
  }
  config.graphics = nullptr;
}

std::unique_ptr<rex::ui::ImmediateDrawer> ReeotApp::OnCreateImmediateDrawer() {
  return eot::gpu::CreateOverlayDrawer();
}

void ReeotApp::OnCreateDialogs(rex::ui::ImGuiDrawer *drawer) {
  drawer->AddDialog(new FpsOverlayDialog(drawer));
}

void ReeotApp::OnPreLaunchModule() {
  EOT_INFO("reeot {} ({}@{}{}) renderer starting", REEOT_VERSION_STRING, REEOT_GIT_BRANCH,
           REEOT_GIT_COMMIT, REEOT_GIT_DIRTY ? "+" : "");
#if defined(_WIN32)
  timeBeginPeriod(1);
#endif
  if (!eot::gpu::Video::CreateHostDevice(window())) {
    EOT_ERROR("Host device creation failed: the guest will run headless");
    return;
  }
  eot::gpu::SetOverlayDrawHook([this](plume::RenderCommandList *cmd,
                                      plume::RenderFramebuffer *framebuffer, uint32_t width,
                                      uint32_t height) {
    if (!imgui_drawer() || !imgui_drawer()->HasDialogs() || eot::gpu::Video::IsShuttingDown())
      return;
    app_context().CallInUIThreadSynchronous([&] {
      eot::gpu::OverlayDrawContext ctx(width, height, cmd, framebuffer);
      imgui_drawer()->Draw(ctx);
    });
  });
  eot::gpu::GuestShadersInit();
  eot::gpu::PsoCachePrecache();
}

void ReeotApp::OnShutdown() {
  eot::gpu::Video::BeginShutdown();
  eot::gpu::Video::Shutdown();
#if defined(_WIN32)
  timeEndPeriod(1);
#endif
}

void ReeotApp::OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) {
  (void)pixel_width;
  (void)pixel_height;
  eot::gpu::Video::RequestResize();
}

bool ReeotApp::OnWindowCloseRequested() {
  eot::gpu::Video::RequestShutdown();
  app_context().ExecutePendingFunctionsFromUIThread();
  eot::QuitProcess(0);
}
