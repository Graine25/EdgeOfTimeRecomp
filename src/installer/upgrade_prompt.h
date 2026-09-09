#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

struct ImGuiIO;

namespace rex::ui {
class ImGuiDrawer;
class WindowedAppContext;
}

namespace eot::installer {

class UpgradePrompt final : public rex::ui::ImGuiDialog {
public:
  using DoneCallback = std::function<void(bool accepted)>;

  UpgradePrompt(rex::ui::ImGuiDrawer *drawer, rex::ui::WindowedAppContext &app_context,
                std::filesystem::path install_root, std::string installed, DoneCallback on_done);

protected:
  void OnDraw(ImGuiIO &io) override;

private:
  void Answer(bool accepted);

  rex::ui::WindowedAppContext &app_context_;
  std::filesystem::path install_root_;
  std::string installed_;
  DoneCallback on_done_;
  bool answered_ = false;
};

}
