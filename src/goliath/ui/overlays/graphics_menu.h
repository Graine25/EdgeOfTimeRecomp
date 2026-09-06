#pragma once

#include <rex/ui/imgui_dialog.h>

#include "imgui.h"

class GraphicsMenuDialog : public rex::ui::ImGuiDialog {
public:
  explicit GraphicsMenuDialog(rex::ui::ImGuiDrawer *drawer) : rex::ui::ImGuiDialog(drawer) {}

  void OnDraw(ImGuiIO &io) override;
};
