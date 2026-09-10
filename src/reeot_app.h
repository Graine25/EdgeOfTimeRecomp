/**
 * @file    reeot_app.h
 * @brief   The reeot host application: brings up the native renderer before
 *          the guest launches and tears it down after. On a machine with no
 *          recorded install it runs the first-run installer first
 *          (installer/installer_wizard.h) and boots from what it installed.
 *
 *          The startup follows reblue's app (BSD 3-Clause, Tom Clay): the
 *          cvar defaults the port moves, the single-instance lock, the build
 *          banner, the crash reporters, the install root with its profiles,
 *          the three answers to where the game is, the offer to copy a newer
 *          build over the install.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <rex/rex_app.h>

#include "installer/installer.h"

class ReeotApp : public rex::ReXApp {
public:
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext &ctx);

  explicit ReeotApp(rex::ui::WindowedAppContext &ctx);
  ~ReeotApp() override;

protected:
  void OnConfigurePaths(rex::PathConfig &paths) override;
  void OnPostInitLogging() override;
  void OnPreSetup(rex::RuntimeConfig &config) override;
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig &defaults,
                                                 std::function<void(rex::PathConfig)> resume) override;
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer *drawer) override;
  void OnConfigureFonts(ImFontAtlas *atlas) override;
  void OnConfigureStyle(ImGuiStyle &imgui_style, rex::ui::Style &ui_style) override;
  void OnPreLaunchModule() override;
  void OnShutdown() override;
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override;
  bool OnWindowCloseRequested() override;

private:
  std::optional<std::filesystem::path> NamedGameFolder() const;
  std::optional<std::filesystem::path> EarlyInstallRoot() const;
  void UseInstallRoot(const std::filesystem::path &root, rex::PathConfig &paths);
  rex::PathConfig PathsForInstall(const rex::PathConfig &defaults, const eot::installer::InstallConfig &cfg);
  bool NeedsUpgradePrompt(const eot::installer::InstallConfig &cfg) const;
  void RestampInstall(const eot::installer::InstallConfig &cfg);

  void InstallOverlayHook();
  bool BeginPreGuestUI();
  void StartPreGuestPump();
  void StopPreGuestPump();
  void QuitNow();

#ifdef REEOT_BUILD_INSTALLER
  void BeginUpgrade(const eot::installer::InstallConfig &cfg, rex::PathConfig defaults,
                    std::function<void(rex::PathConfig)> resume);
  void FinishUpgrade(bool accepted, eot::installer::InstallConfig cfg, rex::PathConfig defaults,
                     std::function<void(rex::PathConfig)> resume);
  void FinishInstaller(rex::PathConfig defaults, std::function<void(rex::PathConfig)> resume, bool completed,
                       const eot::installer::InstallConfig &cfg, const eot::installer::WizardChoices &choices);

  std::unique_ptr<eot::installer::InstallerWizard> installer_wizard_;
  std::unique_ptr<eot::installer::UpgradePrompt> upgrade_prompt_;
#endif

  std::string active_profile_ = "default";
  std::filesystem::path install_root_;
  std::filesystem::path profile_root_;

  std::thread pre_guest_pump_;
  std::atomic<bool> pre_guest_pump_stop_{false};
  std::atomic<bool> pre_guest_pump_exited_{true};
  bool overlay_hook_installed_ = false;
};
