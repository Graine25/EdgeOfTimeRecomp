#include "reeot_app.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/perf/counter.h>
#include <rex/runtime.h>
#include <rex/version.h>

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
#include "platform/crash_handler.h"
#include "platform/desktop_shortcut.h"
#include "platform/fatal_dialog.h"
#include "platform/process.h"
#include "ui/theme.h"

#ifdef REEOT_BUILD_INSTALLER
REXCVAR_DEFINE_BOOL(eot_repair, false, "EdgeOfTime/Config", "Open installer in repair mode");
REXCVAR_DEFINE_BOOL(eot_no_installer, false, "EdgeOfTime/Config", "Never open the installer");
#endif

namespace {

namespace fs = std::filesystem;

constexpr const char *kExecutable = "reeot.exe";

bool GameFolderHolds(const fs::path &folder) {
  std::error_code ec;
  return fs::is_regular_file(folder / "Default.xex", ec);
}

bool SamePlace(const fs::path &a, const fs::path &b) {
  std::error_code ec;
  if (fs::equivalent(a, b, ec))
    return true;
  return a.lexically_normal() == b.lexically_normal();
}

bool SetCvarDefault(std::string_view name, const std::string &value) {
  for (auto &entry : rex::cvar::GetRegistry()) {
    if (entry.name != name)
      continue;
    const bool untouched = !entry.getter || entry.getter() == entry.default_value;
    entry.default_value = value;
    if (untouched && entry.setter)
      entry.setter(value);
    return true;
  }
  return false;
}

void ApplyReeotCvarDefaults() {
  SetCvarDefault("mnk_mode", "true");
  SetCvarDefault("hid_mappings_file",
                 (rex::filesystem::GetExecutableFolder() / "gamecontrollerdb.txt").generic_string());
  SetCvarDefault("log_flush_interval", "1");
}

}

std::unique_ptr<rex::ui::WindowedApp> ReeotApp::Create(rex::ui::WindowedAppContext &ctx) {
  return std::unique_ptr<ReeotApp>(new ReeotApp(ctx));
}

ReeotApp::ReeotApp(rex::ui::WindowedAppContext &ctx) : rex::ReXApp(ctx, "reeot", PPCImageConfig) {
  ApplyReeotCvarDefaults();
}

ReeotApp::~ReeotApp() = default;

void ReeotApp::OnConfigurePaths(rex::PathConfig &paths) {
  (void)paths;
  eot::platform::InstallTerminateHandler();
}

void ReeotApp::OnPostInitLogging() {
  EOT_INFO("reeot v" REEOT_VERSION_STRING " [" REXGLUE_BUILD_CONFIG "] " REEOT_BUILD_PLATFORM);
  EOT_INFO("  commit:  " REEOT_GIT_COMMIT " on " REEOT_GIT_BRANCH "{}",
           REEOT_GIT_DIRTY ? " (local modifications)" : "");
  EOT_INFO("  built:   " REEOT_BUILD_TIMESTAMP " with " REEOT_BUILD_COMPILER);
  EOT_INFO("  sdk:     rexglue-v" REXGLUE_VERSION_STRING " " REXGLUE_BUILD_PLATFORM " @" REXGLUE_BUILD_TIMESTAMP);

  if (!eot::platform::AcquireInstanceLock()) {
    eot::platform::ShowFatalError("reeot is already running", "Close the running copy before starting another.");
    rex::FlushLogging();
    std::_Exit(1);
  }
}

rex::PathConfig ReeotApp::PathsForInstall(const rex::PathConfig &defaults,
                                          const eot::installer::InstallConfig &cfg) {
  EOT_INFO("[install] using the install at {} (recorded by {})", cfg.install_root.string(), cfg.app_version);
  eot::installer::SyncPortPackages(cfg.game_data_path());
  rex::PathConfig paths = defaults;
  paths.game_data_root = cfg.game_data_path();
  return paths;
}

bool ReeotApp::NeedsUpgradePrompt(const eot::installer::InstallConfig &cfg) const {
#ifdef REEOT_BUILD_INSTALLER
  return !SamePlace(eot::platform::ProgramDir(), cfg.install_root);
#else
  (void)cfg;
  return false;
#endif
}

void ReeotApp::RestampInstall(const eot::installer::InstallConfig &cfg) {
  if (!eot::installer::WriteInstallRegistry(cfg))
    EOT_WARN("[install] the record could not be restamped; the update will be offered again");
}

std::optional<rex::PathConfig>
ReeotApp::OnFinalizePaths(const rex::PathConfig &defaults, std::function<void(rex::PathConfig)> resume) {
  fs::path named = defaults.game_data_root;
  if (const std::string cvar = REXCVAR_GET(game_data_root); !cvar.empty())
    named = cvar;
  if (!named.empty()) {
    if (GameFolderHolds(named)) {
      EOT_INFO("[install] game folder {}", named.string());
      rex::PathConfig paths = defaults;
      paths.game_data_root = named;
      return paths;
    }
    EOT_WARN("[install] {} holds no Default.xex; looking for an install instead", named.string());
  }

  bool repair_requested = false;
#ifdef REEOT_BUILD_INSTALLER
  repair_requested = REXCVAR_GET(eot_repair);
  REXCVAR_SET(eot_repair, false);
#endif
  std::optional<eot::installer::InstallConfig> existing;
  if (auto cfg = eot::installer::ReadInstallRegistry()) {
    const bool present = eot::installer::InstallIsPresent(*cfg);
    const bool current = cfg->schema_version == eot::installer::kInstallSchemaVersion;
    if (present && current && !repair_requested) {
      if (cfg->app_version != REEOT_VERSION_STRING) {
        EOT_INFO("[install] the install at {} records version {}, this is {}", cfg->install_root.string(),
                 cfg->app_version, REEOT_VERSION_STRING);
#ifdef REEOT_BUILD_INSTALLER
        if (NeedsUpgradePrompt(*cfg)) {
          if (!BeginPreGuestUI())
            return std::nullopt;
          BeginUpgrade(*cfg, defaults, resume);
          return std::nullopt;
        }
#endif
        RestampInstall(*cfg);
      } else if (!SamePlace(eot::platform::ProgramDir(), cfg->install_root)) {
        EOT_WARN("[install] running from {} rather than the install folder {}",
                 eot::platform::ProgramDir().string(), cfg->install_root.string());
      }
      return PathsForInstall(defaults, *cfg);
    }
    if (repair_requested)
      EOT_INFO("[install] eot_repair: opening the installer in repair mode on {}", cfg->install_root.string());
    else if (!present)
      EOT_WARN("[install] the record names {} but it holds no Default.xex; opening the installer",
               cfg->game_data_path().string());
    else
      EOT_INFO("[install] the record schema {} differs from {}; opening the installer in repair mode on {}",
               cfg->schema_version, eot::installer::kInstallSchemaVersion, cfg->install_root.string());
    existing = std::move(cfg);
  }

#ifdef REEOT_BUILD_INSTALLER
  if (REXCVAR_GET(eot_no_installer)) {
    eot::platform::ShowFatalError("reeot - game not installed",
                                  "No installed game was found and eot_no_installer is set. Run without it "
                                  "to open the installer, or name the game folder with --game_data_root.");
    app_context().QuitFromUIThread();
    return std::nullopt;
  }

  if (!existing)
    EOT_INFO("[install] no game folder and no recorded install; opening the installer");
  if (!BeginPreGuestUI())
    return std::nullopt;
  std::error_code ec;
  const bool repair = existing && fs::is_directory(existing->install_root, ec);
  const fs::path default_install_dir = existing ? existing->install_root : eot::platform::ProgramDir();
  installer_wizard_ = std::make_unique<eot::installer::InstallerWizard>(
      imgui_drawer(), immediate_drawer(), app_context(), default_install_dir, repair,
      existing ? &*existing : nullptr,
      [this, defaults, resume](bool completed, const eot::installer::InstallConfig &cfg,
                               const eot::installer::WizardChoices &choices) {
        FinishInstaller(defaults, resume, completed, cfg, choices);
      });
  return std::nullopt;
#else
  (void)resume;
  eot::platform::ShowFatalError("reeot - game not installed",
                                "No installed game was found, and this build has no installer. Name the "
                                "game folder with --game_data_root.");
  app_context().QuitFromUIThread();
  return std::nullopt;
#endif
}

bool ReeotApp::BeginPreGuestUI() {
  if (!eot::gpu::Video::CreateHostDevice(window())) {
    eot::platform::ShowFatalError("reeot - renderer init failed", "Failed to initialize the renderer.");
    app_context().QuitFromUIThread();
    return false;
  }
  InstallOverlayHook();
  eot::platform::InstallCrashHandler();
  app_context().CallInUIThreadDeferred([] { eot::platform::RaiseMainWindow(); });
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
void ReeotApp::BeginUpgrade(const eot::installer::InstallConfig &cfg, rex::PathConfig defaults,
                            std::function<void(rex::PathConfig)> resume) {
  upgrade_prompt_ = std::make_unique<eot::installer::UpgradePrompt>(
      imgui_drawer(), app_context(), cfg.install_root, cfg.app_version,
      [this, cfg, defaults, resume](bool accepted) { FinishUpgrade(accepted, cfg, defaults, resume); });
}

void ReeotApp::FinishUpgrade(bool accepted, eot::installer::InstallConfig cfg, rex::PathConfig defaults,
                             std::function<void(rex::PathConfig)> resume) {
  StopPreGuestPump();
  upgrade_prompt_.reset();

  if (!accepted) {
    EOT_INFO("[install] update declined; booting the install as it stands");
    eot::platform::UninstallCrashHandler();
    resume(PathsForInstall(defaults, cfg));
    return;
  }

  std::string copy_error;
  if (!eot::installer::CopyProgramTo(cfg.install_root, copy_error)) {
    eot::platform::ShowFatalError("Update failed", "Could not copy " + copy_error + " into " +
                                                       cfg.install_root.string() +
                                                       "\n\nThe installed version is untouched.");
    QuitNow();
  }
  RestampInstall(cfg);
  if (!eot::platform::SpawnReplacement(cfg.install_root / kExecutable))
    eot::platform::ShowFatalError("Updated, could not start it",
                                  std::string("reeot is up to date. Run ") + kExecutable + " from\n" +
                                      cfg.install_root.string());
  QuitNow();
}

void ReeotApp::FinishInstaller(rex::PathConfig defaults, std::function<void(rex::PathConfig)> resume,
                               bool completed, const eot::installer::InstallConfig &cfg,
                               const eot::installer::WizardChoices &choices) {
  StopPreGuestPump();
  installer_wizard_.reset();

  if (!completed)
    QuitNow();

  const fs::path install_root = cfg.install_root;
  const fs::path program_dir = eot::platform::ProgramDir();
  const bool in_place = SamePlace(program_dir, install_root);

  if (!in_place) {
    std::string copy_error;
    if (!eot::installer::CopyProgramTo(install_root, copy_error)) {
      if (eot::platform::ShowFatalErrorWithAction("Install failed",
                                                  "Could not copy " + copy_error + " into " +
                                                      install_root.string() +
                                                      "\n\nNothing was recorded, so a retry starts over.",
                                                  "Retry", window())) {
        if (!eot::platform::RelaunchSelf())
          eot::platform::ShowFatalError("Could not restart",
                                        "Run " + (program_dir / kExecutable).string() + " by hand.");
      }
      QuitNow();
    }
  }

  if (!eot::installer::WriteInstallRegistry(cfg))
    EOT_WARN("[install] the install record could not be written; the installer will show again");
  EOT_INFO("[install] installed to {} (disc {})", install_root.string(), cfg.disc_fingerprint);

  const fs::path config = in_place || defaults.config_path.empty()
                              ? defaults.config_path
                              : install_root / defaults.config_path.filename();
  if (!choices.settings.empty() && !config.empty()) {
    rex::cvar::SaveConfig(config);
    EOT_INFO("[install] {} settings written to {}", choices.settings.size(), config.string());
  }

  if (choices.create_shortcut) {
    std::string shortcut_error;
    if (!eot::platform::CreateDesktopShortcut(install_root / kExecutable, "reeot", shortcut_error))
      EOT_WARN("[install] could not create the desktop shortcut: {}", shortcut_error);
  }

  if (!in_place) {
    if (!eot::platform::SpawnReplacement(install_root / kExecutable))
      eot::platform::ShowFatalError("Install finished, could not start it",
                                    std::string("The game is installed. Run ") + kExecutable + " from\n" +
                                        install_root.string());
    QuitNow();
  }

  eot::platform::UninstallCrashHandler();
  resume(PathsForInstall(defaults, cfg));
}
#endif

void ReeotApp::OnPreSetup(rex::RuntimeConfig &config) {
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
  window()->SetTitle("reeot v" REEOT_VERSION_STRING " " REXGLUE_BUILD_TITLE);
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
  eot::platform::InstallCrashHandler();
#if defined(_WIN32)
  timeBeginPeriod(1);
#endif
  if (!eot::gpu::Video::CreateHostDevice(window())) {
    eot::platform::ShowFatalError("reeot - renderer init failed", "Failed to initialize the renderer.");
    app_context().QuitFromUIThread();
    return;
  }
  InstallOverlayHook();
  app_context().CallInUIThreadDeferred([] { eot::platform::RaiseMainWindow(); });
  eot::gpu::GuestShadersInit();
  eot::gpu::PsoCachePrecache();
}

void ReeotApp::OnShutdown() {
  StopPreGuestPump();
#ifdef REEOT_BUILD_INSTALLER
  installer_wizard_.reset();
  upgrade_prompt_.reset();
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
