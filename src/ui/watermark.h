#pragma once

#include <rex/ui/imgui_dialog.h>

struct ImGuiIO;

namespace rex::ui {
class ImGuiDrawer;
}

namespace eot::ui {

class WatermarkOverlay final : public rex::ui::ImGuiDialog {
public:
  explicit WatermarkOverlay(rex::ui::ImGuiDrawer *drawer) : rex::ui::ImGuiDialog(drawer) {}

protected:
  void OnDraw(ImGuiIO &io) override;
};

}
