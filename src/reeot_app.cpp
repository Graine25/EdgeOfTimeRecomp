#include "reeot_app.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/perf/counter.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/achievement_toast.h>
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
#include "goliath/controller/pc_controls.h"
#include "goliath/text/translation.h"
#include "goliath/ui/overlays/fps.h"
#include "ui/watermark.h"
#include "platform/update_check.h"
#include "installer/uninstall.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "platform/crash_handler.h"
#include "platform/desktop_shortcut.h"
#include "platform/fatal_dialog.h"
#include "platform/language.h"
#include "platform/process.h"
#include "ui/theme.h"

REXCVAR_DEFINE_STRING(profile, "default", "EdgeOfTime/Config", "Active profile name")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#ifdef REEOT_BUILD_INSTALLER
REXCVAR_DEFINE_BOOL(repair, false, "EdgeOfTime/Config", "Open installer in repair mode");
REXCVAR_DEFINE_BOOL(eot_no_installer, false, "EdgeOfTime/Config", "Never open the installer");
#endif
REXCVAR_DEFINE_BOOL(uninstall, false, "EdgeOfTime/Config", "Uninstall and keep saves");

REXCVAR_DEFINE_BOOL(eot_achievement_notifications, true, "EdgeOfTime/Config", "Show achievement pop-ups");
REXCVAR_DEFINE_BOOL(eot_background_input, false, "EdgeOfTime/Input", "Controller works in background");
REXCVAR_DEFINE_STRING(eot_language, "auto", "EdgeOfTime/Config", "Language the game uses")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
std::filesystem::path g_config_path;

void ApplyBackgroundInput(bool on) {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, on ? "1" : "0");
  EOT_INFO("[input] controller input in the background: {}", on ? "on" : "off");
}

class GatedAchievementToast final : public rex::ui::AchievementToastDialog {
public:
  using AchievementToastDialog::AchievementToastDialog;
  void Push(const rex::system::AchievementEvent &event) override {
    if (REXCVAR_GET(eot_achievement_notifications))
      AchievementToastDialog::Push(event);
    else
      EOT_INFO("[achievements] unlocked with the notifications off");
  }
};
}

REXCVAR_DEFINE_COMMAND(
    eot_save_settings,
    []() {
      if (g_config_path.empty()) {
        EOT_WARN("[settings] no profile config to save to");
        return;
      }
      rex::cvar::SaveConfig(g_config_path);
      EOT_INFO("[settings] saved to {}", g_config_path.string());
    },
    "EdgeOfTime/Config", "Save settings to profile");

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

bool SetCvarValue(std::string_view name, const std::string &value) {
  for (auto &entry : rex::cvar::GetRegistry()) {
    if (entry.name != name)
      continue;
    if (!entry.setter)
      return false;
    entry.setter(value);
    return true;
  }
  return false;
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
  SetCvarDefault("mnk_mode", "false");
  SetCvarDefault("hid_mappings_file",
                 (rex::filesystem::GetExecutableFolder() / "gamecontrollerdb.txt").generic_string());
  SetCvarDefault("log_flush_interval", "1");
  SetCvarDefault("license_mask", "1");
}

std::string SanitizeProfileName(const std::string &raw) {
  std::string out;
  for (char c : raw) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || c == '/' || c == '\\' || c == ':')
      continue;
    out += c;
  }
  const auto b = out.find_first_not_of(" .");
  const auto e = out.find_last_not_of(" .");
  if (b == std::string::npos)
    return "default";
  out = out.substr(b, e - b + 1);
  if (out.empty() || out == "." || out == "..")
    return "default";
  return out;
}

}

std::unique_ptr<rex::ui::WindowedApp> ReeotApp::Create(rex::ui::WindowedAppContext &ctx) {
  return std::unique_ptr<ReeotApp>(new ReeotApp(ctx));
}

ReeotApp::ReeotApp(rex::ui::WindowedAppContext &ctx) : rex::ReXApp(ctx, "reeot", PPCImageConfig) {
  ApplyReeotCvarDefaults();
}

ReeotApp::~ReeotApp() = default;

std::optional<fs::path> ReeotApp::NamedGameFolder() const {
  std::string named = REXCVAR_GET(game_data_root);
  if (named.empty())
    if (auto positional = GetArgument("game_directory"))
      named = *positional;
  if (named.empty() || !GameFolderHolds(named))
    return std::nullopt;
  return fs::absolute(named);
}

std::optional<fs::path> ReeotApp::EarlyInstallRoot() const {
  if (auto named = NamedGameFolder())
    return named->parent_path();
  if (auto cfg = eot::installer::ReadInstallRegistry())
    if (cfg->schema_version == eot::installer::kInstallSchemaVersion && eot::installer::InstallIsPresent(*cfg))
      return cfg->install_root;
  return std::nullopt;
}

void ReeotApp::UseInstallRoot(const fs::path &root, rex::PathConfig &paths) {
  install_root_ = root;
  profile_root_ = root / "profiles" / active_profile_;
  std::error_code ec;
  fs::create_directories(profile_root_, ec);
  paths.user_data_root = profile_root_;
  paths.cache_root = profile_root_ / "cache";
  paths.config_path = profile_root_ / "reeot.toml";
  g_config_path = paths.config_path;
}

void ReeotApp::OnConfigurePaths(rex::PathConfig &paths) {
  eot::platform::InstallTerminateHandler();

  active_profile_ = SanitizeProfileName(REXCVAR_GET(profile));
  REXCVAR_SET(profile, "default");

  const std::optional<fs::path> root = EarlyInstallRoot();
  if (!root) {
    paths.config_path.clear();
    return;
  }
  UseInstallRoot(*root, paths);
}

void ReeotApp::OnPostInitLogging() {
  EOT_INFO("reeot v" REEOT_VERSION_STRING " [" REXGLUE_BUILD_CONFIG "] " REEOT_BUILD_PLATFORM);
  EOT_INFO("  commit:  " REEOT_GIT_COMMIT " on " REEOT_GIT_BRANCH "{}",
           REEOT_GIT_DIRTY ? " (local modifications)" : "");
  EOT_INFO("  built:   " REEOT_BUILD_TIMESTAMP " with " REEOT_BUILD_COMPILER);
  EOT_INFO("  sdk:     rexglue-v" REXGLUE_VERSION_STRING " " REXGLUE_BUILD_PLATFORM " @" REXGLUE_BUILD_TIMESTAMP);
  if (!install_root_.empty())
    EOT_INFO("  profile: {} ({})", active_profile_, profile_root_.string());

  if (REXCVAR_GET(uninstall)) {
    eot::installer::RunUninstall();
    rex::FlushLogging();
    std::_Exit(0);
  }

#ifdef REEOT_BUILD_INSTALLER
  repair_requested_ = REXCVAR_GET(repair);
  REXCVAR_SET(repair, false);
#endif

  {
    const std::string wanted = REXCVAR_GET(eot_language);
    const uint32_t xlanguage = eot::platform::XLanguageFor(wanted);
    rex::cvar::SetFlagByName("user_language", std::to_string(xlanguage));
    EOT_INFO("[language] eot_language {} -> {} (XLanguage {})", wanted, eot::platform::XLanguageName(xlanguage),
             xlanguage);
  }

  eot::platform::UpdateInstalledCopy(install_root_);
  if (const auto done = eot::platform::LastInstallUpdate())
    eot::platform::ShowInfo("reeot updated", "reeot " + done->version + " is now installed in\n" +
                                                 done->location +
                                                 "\n\nYour installed copy is up to date.");

  if (!eot::platform::AcquireInstanceLock()) {
    eot::platform::ShowFatalError("reeot is already running", "Close the running copy before starting another.");
    rex::FlushLogging();
    std::_Exit(1);
  }
}

void ReeotApp::LoadTranslation(const std::filesystem::path &game) {
  const std::string tag = eot::platform::TranslationTag(REXCVAR_GET(eot_language));
  if (eot::text::LoadTranslation(game, tag))
    EOT_INFO("[language] the {} translation is in: {} lines over the game's text", tag,
             eot::text::TranslatedLines());
}

rex::PathConfig ReeotApp::PathsForInstall(const rex::PathConfig &defaults,
                                          const eot::installer::InstallConfig &cfg) {
  rex::PathConfig paths = defaults;
  UseInstallRoot(cfg.install_root, paths);
  paths.game_data_root = cfg.game_data_path();
  eot::installer::WritePortFiles(paths.game_data_root);
  LoadTranslation(paths.game_data_root);
  eot::installer::PublishDlc(cfg.install_root / eot::installer::kDlcFolderName, paths.game_data_root,
                             profile_root_);
  EOT_INFO("[install] using the install at {} (recorded by {}; profile {})", cfg.install_root.string(),
           cfg.app_version, profile_root_.string());
  return paths;
}

std::optional<rex::PathConfig>
ReeotApp::OnFinalizePaths(const rex::PathConfig &defaults, std::function<void(rex::PathConfig)> resume) {
  if (auto named = NamedGameFolder()) {
    EOT_INFO("[install] game folder {}", named->string());
    eot::installer::WritePortFiles(*named);
    LoadTranslation(*named);
    eot::installer::PublishDlc(install_root_ / eot::installer::kDlcFolderName, *named, profile_root_);
    rex::PathConfig paths = defaults;
    paths.game_data_root = *named;
    return paths;
  }
  if (const std::string named = REXCVAR_GET(game_data_root); !named.empty())
    EOT_WARN("[install] {} holds no Default.xex; looking for an install instead", named);

  const bool repair_requested = repair_requested_;
  std::optional<eot::installer::InstallConfig> existing;
  if (auto cfg = eot::installer::ReadInstallRegistry()) {
    const bool present = eot::installer::InstallIsPresent(*cfg);
    const bool current = cfg->schema_version == eot::installer::kInstallSchemaVersion;
    if (present && current && !repair_requested)
      return PathsForInstall(defaults, *cfg);
    if (repair_requested)
      EOT_INFO("[install] --repair: opening the installer in repair mode on {}", cfg->install_root.string());
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
  const fs::path default_install_dir =
      existing ? existing->install_root : eot::installer::DefaultInstallRoot();
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
                                                  "Could not copy the program into " + install_root.string() +
                                                      ": " + copy_error +
                                                      "\n\nNothing was recorded, so a retry starts over.",
                                                  "Retry", window())) {
        if (!eot::platform::RelaunchSelf())
          eot::platform::ShowFatalError("Could not restart",
                                        "Run " + (program_dir / kExecutable).string() + " by hand.");
      }
      QuitNow();
    }
  }

  eot::installer::WritePortFiles(cfg.game_data_path());
  if (!eot::installer::WriteInstallRegistry(cfg))
    EOT_WARN("[install] the install record could not be written; the installer will show again");
  EOT_INFO("[install] installed to {} (disc {})", install_root.string(), cfg.disc_fingerprint);

  rex::PathConfig paths = defaults;
  UseInstallRoot(install_root, paths);
  if (!choices.settings.empty()) {
    std::error_code ec;
    if (fs::exists(paths.config_path, ec))
      rex::cvar::LoadConfig(paths.config_path);
    for (const auto &pick : choices.settings)
      rex::cvar::SetFlagByName(pick.cvar, pick.value);
    rex::cvar::SaveConfig(paths.config_path);
    EOT_INFO("[install] {} settings written to {}", choices.settings.size(), paths.config_path.string());
  }
  eot::installer::AdoptLegacyUserData(profile_root_);
  eot::installer::PublishDlc(install_root / eot::installer::kDlcFolderName, cfg.game_data_path(), profile_root_);

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

std::unique_ptr<rex::ui::AchievementNotificationDialog> ReeotApp::CreateAchievementNotificationDialog() {
  if (!imgui_drawer() || !immediate_drawer() || !runtime())
    return nullptr;
  return std::make_unique<GatedAchievementToast>(imgui_drawer(), immediate_drawer(), runtime());
}

void ReeotApp::OnPreSetup(rex::RuntimeConfig &config) {
  eot::goliath::InstallPcControls();
  ApplyBackgroundInput(REXCVAR_GET(eot_background_input));
  rex::cvar::RegisterChangeCallback("eot_background_input", [](std::string_view, std::string_view value) {
    ApplyBackgroundInput(value == "true" || value == "1");
  });
  if (rex::cvar::Query<bool>("eot_debug_mode"))
    SetCvarValue("mnk_mode", "false");

  SetCvarValue("eot_debug_pause", "false");
  SetCvarValue("eot_freecam", "false");
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
  EOT_INFO("[window] {}x{}, {}", window()->GetActualPhysicalWidth(), window()->GetActualPhysicalHeight(),
           window()->IsFullscreen() ? "fullscreen" : "windowed");
  drawer->AddDialog(new FpsOverlayDialog(drawer));
  rex::ui::RegisterBind("bind_fps_overlay", "F8", "Toggle the FPS overlay", [] {
    const bool shown = rex::cvar::Query<bool>("show_fps_overlay");
    rex::cvar::SetFlagByName("show_fps_overlay", shown ? "false" : "true");
  });
  drawer->AddDialog(new eot::ui::WatermarkOverlay(drawer));
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
