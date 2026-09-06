#include "goliath/ui/overlays/graphics_menu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/cvar.h>

#include "goliath/ui/menu_handles.h"

REXCVAR_DEFINE_BOOL(eot_graphics_menu, false, "eot",
                    "The in-game Graphics menu is open (the Options bar's Graphics entry sets it; "
                    "Back clears it).");

namespace {

using rex::cvar::FlagEntry;
using rex::cvar::FlagType;

const char *const kSections[] = {"EdgeOfTime/Video", "EdgeOfTime/Graphics"};

std::string Label(const FlagEntry &e) {
  if (e.description.empty())
    return e.name;
  const size_t dot = e.description.find(". ");
  std::string s = dot == std::string::npos ? e.description : e.description.substr(0, dot);
  const size_t paren = s.find(" (");
  if (paren != std::string::npos && paren > 8)
    s = s.substr(0, paren);
  return s;
}

void Tooltip(const FlagEntry &e) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    ImGui::SetTooltip("%s\n\n%s (default %s)", e.description.c_str(), e.name.c_str(),
                      e.default_value.c_str());
}

int ComboIndex(const std::vector<std::string> &values, const std::string &current) {
  for (size_t i = 0; i < values.size(); ++i)
    if (values[i] == current)
      return static_cast<int>(i);
  return -1;
}

void DrawEntry(const FlagEntry &e) {
  ImGui::PushID(e.name.c_str());
  const std::string label = Label(e);
  const std::string current = e.getter();
  const auto &c = e.constraints;
  const bool read_only = e.lifecycle == rex::cvar::Lifecycle::kInitOnly;
  if (read_only)
    ImGui::BeginDisabled();

  if (c.HasAllowedValues()) {
    int idx = ComboIndex(c.allowed_values, current);
    const char *preview = idx >= 0 ? c.allowed_values[idx].c_str() : current.c_str();
    if (ImGui::BeginCombo(label.c_str(), preview)) {
      for (size_t i = 0; i < c.allowed_values.size(); ++i) {
        const bool selected = static_cast<int>(i) == idx;
        if (ImGui::Selectable(c.allowed_values[i].c_str(), selected))
          e.setter(c.allowed_values[i]);
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  } else {
    switch (e.type) {
    case FlagType::Boolean: {
      bool v = current == "true";
      if (ImGui::Checkbox(label.c_str(), &v))
        e.setter(v ? "true" : "false");
      break;
    }
    case FlagType::Int32:
    case FlagType::Int64:
    case FlagType::Uint32:
    case FlagType::Uint64: {
      int v = std::atoi(current.c_str());
      bool changed = false;
      if (c.HasRangeConstraint()) {
        const int lo = static_cast<int>(c.min.value_or(0.0)), hi = static_cast<int>(c.max.value_or(100.0));
        changed = ImGui::SliderInt(label.c_str(), &v, lo, hi);
      } else {
        changed = ImGui::InputInt(label.c_str(), &v);
      }
      if (changed)
        e.setter(std::to_string(v));
      break;
    }
    case FlagType::Double: {
      double v = std::atof(current.c_str());
      bool changed = false;
      if (c.HasRangeConstraint()) {
        float f = static_cast<float>(v);
        changed = ImGui::SliderFloat(label.c_str(), &f, static_cast<float>(c.min.value_or(0.0)),
                                     static_cast<float>(c.max.value_or(1.0)), "%.2f");
        v = f;
      } else {
        changed = ImGui::InputDouble(label.c_str(), &v, 0.05, 0.25, "%.2f");
      }
      if (changed) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", v);
        e.setter(buf);
      }
      break;
    }
    case FlagType::String: {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", current.c_str());
      if (ImGui::InputText(label.c_str(), buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
        e.setter(buf);
      break;
    }
    default:
      ImGui::TextDisabled("%s: %s", label.c_str(), current.c_str());
      break;
    }
  }
  Tooltip(e);
  if (read_only)
    ImGui::EndDisabled();
  ImGui::PopID();
}

void Close() { rex::cvar::SetFlagByName(eot::ui::kGraphicsMenuCvar, "false"); }

}

void GraphicsMenuDialog::OnDraw(ImGuiIO &io) {
  if (!REXCVAR_GET(eot_graphics_menu))
    return;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    Close();
    return;
  }
  const ImVec2 ds = io.DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(820.0f, ds.x * 0.85f), std::min(680.0f, ds.y * 0.85f)),
                           ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.94f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("Graphics", nullptr, flags)) {
    ImGui::TextDisabled("Changes apply immediately. Back, Esc or B returns to the game's menu.");
    ImGui::Separator();
    ImGui::BeginChild("##options", ImVec2(0, -36.0f), false);
    auto &registry = rex::cvar::GetRegistry();
    for (const char *section : kSections) {
      ImGui::SeparatorText(section + std::strlen("EdgeOfTime/"));
      for (const auto &e : registry)
        if (e.category == section && !e.is_debug_only)
          DrawEntry(e);
      ImGui::Spacing();
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (ImGui::Button("Back", ImVec2(120.0f, 0.0f)))
      Close();
  }
  ImGui::End();
}
