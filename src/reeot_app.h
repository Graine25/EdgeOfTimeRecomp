#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <rex/rex_app.h>

class ReeotApp : public rex::ReXApp {
public:
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext &ctx);

  explicit ReeotApp(rex::ui::WindowedAppContext &ctx);
  ~ReeotApp() override;

protected:
  void OnPreSetup(rex::RuntimeConfig &config) override;
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig &defaults,
                                                 std::function<void(rex::PathConfig)> resume) override;
  void OnPreLaunchModule() override;
  void OnShutdown() override;
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override;
  bool OnWindowCloseRequested() override;
};
