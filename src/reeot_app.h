#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include <rex/rex_app.h>

#include "installer/installer.h"

class ReeotApp : public rex::ReXApp {
public:
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext &ctx);

  explicit ReeotApp(rex::ui::WindowedAppContext &ctx);
  ~ReeotApp() override;

protected:
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
  void InstallOverlayHook();
  bool BeginPreGuestUI();
  void StartPreGuestPump();
  void StopPreGuestPump();
  void QuitNow();

#ifdef REEOT_BUILD_INSTALLER
  void FinishInstaller(rex::PathConfig defaults, std::function<void(rex::PathConfig)> resume, bool completed,
                       const eot::installer::InstallConfig &cfg, const eot::installer::WizardChoices &choices);

  std::unique_ptr<eot::installer::InstallerWizard> installer_wizard_;
#endif

  std::thread pre_guest_pump_;
  std::atomic<bool> pre_guest_pump_stop_{false};
  std::atomic<bool> pre_guest_pump_exited_{true};
  bool overlay_hook_installed_ = false;
};
