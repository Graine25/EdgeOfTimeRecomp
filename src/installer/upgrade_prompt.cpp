#include "installer/upgrade_prompt.h"

#include <cfloat>
#include <utility>

#include <imgui.h>

#include <rex/ui/windowed_app_context.h>

#include "core/build_info.h"

namespace eot::installer {

UpgradePrompt::UpgradePrompt(rex::ui::ImGuiDrawer *drawer, rex::ui::WindowedAppContext &app_context,
                             std::filesystem::path install_root, std::string installed, DoneCallback on_done)
    : rex::ui::ImGuiDialog(drawer), app_context_(app_context), install_root_(std::move(install_root)),
      installed_(std::move(installed)), on_done_(std::move(on_done)) {}

void UpgradePrompt::Answer(bool accepted) {
  if (answered_)
    return;
  answered_ = true;
  auto cb = on_done_;
  app_context_.CallInUIThreadDeferred([cb, accepted] { cb(accepted); });
}

void UpgradePrompt::OnDraw(ImGuiIO &) {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(620, 0), ImVec2(620, FLT_MAX));
  if (!ImGui::Begin("reeot - Update the install", nullptr,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  ImGui::SetWindowFontScale(1.35f);

  const std::string made_by = installed_.empty() ? "an unknown build" : "reeot " + installed_;
  const std::string body = "The install at " + install_root_.string() + " was made by " + made_by +
                           ". This is reeot " REEOT_VERSION_STRING ". Copy this build into the install folder?";
  ImGui::TextWrapped("%s", body.c_str());
  ImGui::Spacing();
  ImGui::TextWrapped("%s", "The game files stay as they are. reeot then starts again from the install folder.");
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::BeginDisabled(answered_);
  if (ImGui::Button("Update", ImVec2(160, 0)))
    Answer(true);
  ImGui::SameLine();
  if (ImGui::Button("Not now", ImVec2(160, 0)))
    Answer(false);
  ImGui::EndDisabled();
  ImGui::End();
}

}
