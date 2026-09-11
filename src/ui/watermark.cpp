#include "ui/watermark.h"

#include <format>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/version.h>

#include "core/build_info.h"
#include "gpu/device.h"
#include "gpu/settings.h"
#include "platform/update_check.h"

REXCVAR_DEFINE_BOOL(eot_watermark, true, "EdgeOfTime/Video",
                    "Show the build watermark and offline update notice in the corner.");

namespace eot::ui {

namespace {

std::string Resolution() {
  const u32 rw = gpu::InternalRenderWidth(), rh = gpu::InternalRenderHeight();
  if (!rw || !rh)
    return {};
  std::string s = std::format("{}x{}", rw, rh);
  const u32 ow = gpu::Video::OutputWidth(), oh = gpu::Video::OutputHeight();
  if (ow && oh && (ow != rw || oh != rh))
    s += std::format(" -> {}x{}", ow, oh);
  return s;
}

}

void WatermarkOverlay::OnDraw(ImGuiIO &io) {
  if (!REXCVAR_GET(eot_watermark))
    return;

  constexpr ImU32 kText = IM_COL32(255, 255, 255, 96);
  constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 112);
  constexpr ImU32 kNotice = IM_COL32(255, 216, 96, 232);

  std::vector<std::pair<std::string, ImU32>> lines;
  if (const auto update = platform::NewerBuildAvailable()) {
    const std::string what =
        (!update->version.empty() && update->version != REEOT_VERSION_STRING)
            ? std::format("reeot {}", update->version)
            : std::format("build {}", update->stamp);
    lines.emplace_back(std::format("Update available: {} in reeot-dis", what), kNotice);
  }

  lines.emplace_back(std::string("reeot v" REEOT_VERSION_STRING " (" REEOT_GIT_COMMIT) +
                         (REEOT_GIT_DIRTY ? "*)" : ")"),
                     kText);
  lines.emplace_back(std::string("built " REEOT_BUILD_TIMESTAMP), kText);
  lines.emplace_back(std::string("rexglue v" REXGLUE_VERSION_STRING), kText);
  if (std::string res = Resolution(); !res.empty())
    lines.emplace_back(std::move(res), kText);

  constexpr float kPad = 10.0f;
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const float line_h = ImGui::GetTextLineHeight();
  float y = io.DisplaySize.y - kPad - line_h * static_cast<float>(lines.size());
  for (const auto &[text, color] : lines) {
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pos(io.DisplaySize.x - kPad - size.x, y);
    dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), kShadow, text.c_str());
    dl->AddText(pos, color, text.c_str());
    y += line_h;
  }
}

}
