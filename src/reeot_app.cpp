#include "reeot_app.h"

#include <chrono>
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
#include "platform/desktop_shortcut.h"
#include "platform/process.h"
#include "ui/theme.h"

REXCVAR_DECLARE(bool, mnk_mode);

std::unique_ptr<rex::ui::WindowedApp> ReeotApp::Create(rex::ui::WindowedAppContext &ctx) {
  return std::unique_ptr<ReeotApp>(new ReeotApp(ctx));
}

ReeotApp::ReeotApp(rex::ui::WindowedAppContext &ctx) : rex::ReXApp(ctx, "reeot", PPCImageConfig) {}

ReeotApp::~ReeotApp() = default;

REXCVAR_DEFINE_STRING(eot_data_root, "", "EdgeOfTime/Config",
                      "Game data directory (the folder holding Default.xex); the "
                      "config-file alternative to --game_data_root. Set, it bypasses the "
                      "recorded install and the installer.");
#ifdef REEOT_BUILD_INSTALLER
REXCVAR_DEFINE_BOOL(eot_repair, false, "EdgeOfTime/Config",
                    "Open the installer in repair mode on the recorded install at the next "
                    "start, to add the title update or DLC or to restore missing files.");
REXCVAR_DEFINE_BOOL(eot_no_installer, false, "EdgeOfTime/Config",
                    "Never open the installer: with no recorded install and no game folder "
                    "given, quit with a message instead.");
#endif

namespace {

constexpr const char *kExecutable = "reeot.exe";

}

std::optional<rex::PathConfig>
ReeotApp::OnFinalizePaths(const rex::PathConfig &defaults, std::function<void(rex::PathConfig)> resume) {
  rex::PathConfig paths = defaults;

  const std::string root = REXCVAR_GET(eot_data_root);
  if (!root.empty())
    paths.game_data_root = root;
  if (!paths.game_data_root.empty()) {
    (void)resume;
    return paths;
  }

  bool repair_requested = false;
#ifdef REEOT_BUILD_INSTALLER
  repair_requested = REXCVAR_GET(eot_repair);
  REXCVAR_SET(eot_repair, false);
#endif
  std::optional<eot::installer::InstallConfig> existing;
  if (auto cfg = eot::installer::ReadInstallRegistry()) {
    if (cfg->schema_version == eot::installer::kInstallSchemaVersion && !repair_requested) {
      EOT_INFO("[install] using the install at {} (recorded by {})", cfg->install_root.string(),
               cfg->app_version);
      eot::installer::SyncPortPackages(cfg->game_data_path());
      paths.game_data_root = cfg->game_data_path();
      return paths;
    }
    EOT_INFO("[install] {}: opening the installer in repair mode on {}",
             repair_requested ? "eot_repair" : "install record schema differs", cfg->install_root.string());
    existing = std::move(cfg);
  }

#ifdef REEOT_BUILD_INSTALLER
  if (REXCVAR_GET(eot_no_installer)) {
    EOT_ERROR("[install] no game folder and no recorded install; eot_no_installer is set, quitting");
    return paths;
  }
  if (!BeginPreGuestUI())
    return paths;
  const bool repair = existing.has_value();
  const std::filesystem::path default_install_dir =
      repair ? existing->install_root : eot::platform::ProgramDir();
  installer_wizard_ = std::make_unique<eot::installer::InstallerWizard>(
      imgui_drawer(), immediate_drawer(), app_context(), default_install_dir, repair,
      existing ? &*existing : nullptr,
      [this, defaults, resume](bool completed, const eot::installer::InstallConfig &cfg,
                               const eot::installer::WizardChoices &choices) {
        FinishInstaller(defaults, resume, completed, cfg, choices);
      });
  return std::nullopt;
#else
  EOT_ERROR("[install] no game folder and no recorded install, and this build has no installer");
  return paths;
#endif
}

bool ReeotApp::BeginPreGuestUI() {
  if (!eot::gpu::Video::CreateHostDevice(window())) {
    EOT_ERROR("Host device creation failed; the setup cannot be shown");
    return false;
  }
  InstallOverlayHook();
  StartPreGuestPump();
  return true;
}

void ReeotApp::StartPreGuestPump() {
  if (pre_guest_pump_.joinable())
    StopPreGuestPump();
  pre_guest_pump_stop_.store(false);
  pre_guest_pump_exited_.store(false);
  pre_guest_pump_ = std::thread([this] {
    while (!pre_guest_pump_stop_.load(std::memory_order_acquire) && !eot::gpu::Video::IsShuttingDown()) {
      eot::gpu::Video::PresentOverlayOnly();
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    pre_guest_pump_exited_.store(true, std::memory_order_release);
  });
}

void ReeotApp::StopPreGuestPump() {
  if (!pre_guest_pump_.joinable())
    return;
  pre_guest_pump_stop_.store(true, std::memory_order_release);
  while (!pre_guest_pump_exited_.load(std::memory_order_acquire)) {
    app_context().ExecutePendingFunctionsFromUIThread();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  pre_guest_pump_.join();
}

void ReeotApp::QuitNow() {
  eot::gpu::Video::RequestShutdown();
  app_context().ExecutePendingFunctionsFromUIThread();
  eot::QuitProcess(0);
}

#ifdef REEOT_BUILD_INSTALLER
void ReeotApp::FinishInstaller(rex::PathConfig defaults, std::function<void(rex::PathConfig)> resume,
                               bool completed, const eot::installer::InstallConfig &cfg,
                               const eot::installer::WizardChoices &choices) {
  StopPreGuestPump();
  installer_wizard_.reset();

  if (!completed)
    QuitNow();

  const std::filesystem::path install_root = cfg.install_root;
  const std::filesystem::path program_dir = eot::platform::ProgramDir();
  std::error_code ec;
  const bool in_place = std::filesystem::equivalent(program_dir, install_root, ec);

  std::string copy_error;
  bool copied = in_place;
  if (!in_place) {
    copied = eot::installer::CopyProgramTo(install_root, copy_error);
    if (!copied)
      EOT_ERROR("[install] could not copy the program into {}: {}; booting from here this once",
                install_root.string(), copy_error);
  }

  if (!eot::installer::WriteInstallRegistry(cfg))
    EOT_WARN("[install] the install record could not be written; the installer will show again");
  EOT_INFO("[install] installed to {} (disc {})", install_root.string(), cfg.disc_fingerprint);

  const std::filesystem::path config =
      copied && !in_place && !defaults.config_path.empty() ? install_root / defaults.config_path.filename()
                                                           : defaults.config_path;
  if (!choices.settings.empty() && !config.empty()) {
    rex::cvar::SaveConfig(config);
    EOT_INFO("[install] {} settings written to {}", choices.settings.size(), config.string());
  }

  if (choices.create_shortcut) {
    std::string shortcut_error;
    const std::filesystem::path exe = (copied ? install_root : program_dir) / kExecutable;
    if (!eot::platform::CreateDesktopShortcut(exe, "reeot", shortcut_error))
      EOT_WARN("[install] could not create the desktop shortcut: {}", shortcut_error);
  }

  if (copied && !in_place) {
    if (!eot::platform::SpawnReplacement(install_root / kExecutable))
      EOT_ERROR("[install] the game is installed but {} could not be started; run it from {}", kExecutable,
                install_root.string());
    QuitNow();
  }

  rex::PathConfig paths = defaults;
  paths.game_data_root = cfg.game_data_path();
  resume(std::move(paths));
}
#endif

void ReeotApp::OnPreSetup(rex::RuntimeConfig &config) {
  REXCVAR_SET(mnk_mode, true);
  eot::goliath::InstallPcControls();
  if (eot::gpu::Settings::Profiler()) {
#if defined(EOT_PROFILING)
    rex::perf::Profiler::Startup();
    if (rex::perf::Profiler::is_enabled())
      EOT_INFO("Tracy profiler started; connect a viewer to capture.");
    else
      EOT_WARN("eot_profiler is set, but the SDK in this build has no profiler compiled in.");
#else
    EOT_WARN("eot_profiler is set, but this Release build has no zones compiled in: configure "
             "with -DREEOT_PROFILING=ON.");
#endif
  }
  config.graphics = nullptr;
}

std::unique_ptr<rex::ui::ImmediateDrawer> ReeotApp::OnCreateImmediateDrawer() {
  return eot::gpu::CreateOverlayDrawer();
}

void ReeotApp::OnCreateDialogs(rex::ui::ImGuiDrawer *drawer) {
  drawer->AddDialog(new FpsOverlayDialog(drawer));
}

void ReeotApp::OnConfigureFonts(ImFontAtlas *atlas) {
#ifdef REEOT_BUILD_INSTALLER
  eot::installer::InitInstallerFonts(atlas);
#else
  (void)atlas;
#endif
}

void ReeotApp::OnConfigureStyle(ImGuiStyle &imgui_style, rex::ui::Style &ui_style) {
  eot::ui::Theme::Apply(imgui_style, ui_style);
}

void ReeotApp::InstallOverlayHook() {
  if (overlay_hook_installed_)
    return;
  overlay_hook_installed_ = true;
  eot::gpu::SetOverlayDrawHook([this](plume::RenderCommandList *cmd, plume::RenderFramebuffer *framebuffer,
                                      uint32_t width, uint32_t height) {
    if (!imgui_drawer() || !imgui_drawer()->HasDialogs() || eot::gpu::Video::IsShuttingDown())
      return;
    app_context().CallInUIThreadSynchronous([&] {
      eot::gpu::OverlayDrawContext ctx(width, height, cmd, framebuffer);
      imgui_drawer()->Draw(ctx);
    });
  });
}

void ReeotApp::OnPreLaunchModule() {
  EOT_INFO("reeot {} ({}@{}{}) renderer starting", REEOT_VERSION_STRING, REEOT_GIT_BRANCH, REEOT_GIT_COMMIT,
           REEOT_GIT_DIRTY ? "+" : "");
#if defined(_WIN32)
  timeBeginPeriod(1);
#endif
  if (!eot::gpu::Video::CreateHostDevice(window())) {
    EOT_ERROR("Host device creation failed: the guest will run headless");
    return;
  }
  InstallOverlayHook();
  eot::gpu::GuestShadersInit();
  eot::gpu::PsoCachePrecache();
}

void ReeotApp::OnShutdown() {
  StopPreGuestPump();
#ifdef REEOT_BUILD_INSTALLER
  installer_wizard_.reset();
#endif
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
  QuitNow();
  return true;
}
