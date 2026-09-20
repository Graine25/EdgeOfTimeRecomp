#pragma once

#include <rex/ui/imgui_dialog.h>

#include "imgui.h"

bool FpsOverlayEnabled();

class FpsOverlayDialog : public rex::ui::ImGuiDialog {
public:
  explicit FpsOverlayDialog(rex::ui::ImGuiDrawer *drawer) : rex::ui::ImGuiDialog(drawer) {}

  void SyncEnabledState();
  void OnDraw(ImGuiIO &io) override;

private:
  void DrawFpsBlock();
  void DrawGpuBlock(bool below_fps);

  bool registered_ = true;
};
