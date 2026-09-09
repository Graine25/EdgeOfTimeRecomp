#include "installer/installer_wizard.h"

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <stb_image.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <functional>

#include "core/encoding.h"
#include "core/logging.h"
#include "embedded.h"
#include "platform/file_dialog.h"
#include "platform/process.h"
#include "ui/theme.h"

REXCVAR_DEFINE_STRING(eot_install_disc, "", "EdgeOfTime/Config",
                      "Disc image the installer starts with selected.");
REXCVAR_DEFINE_STRING(eot_install_update, "", "EdgeOfTime/Config",
                      "Title update package the installer starts with selected.");
REXCVAR_DEFINE_STRING(eot_install_dlc, "", "EdgeOfTime/Config",
                      "Content packages the installer starts with selected, separated by semicolons.");
REXCVAR_DEFINE_STRING(eot_install_dir, "", "EdgeOfTime/Config",
                      "Install folder the installer starts with selected.");
REXCVAR_DEFINE_BOOL(eot_install_unattended, false, "EdgeOfTime/Config",
                    "With every source given by the eot_install_* settings, install and continue "
                    "without waiting for a click.");

namespace eot::installer {
namespace {

ImFont *g_body_font = nullptr;
ImFont *g_title_font = nullptr;
ImFont *g_path_font = nullptr;

constexpr const char *kTitleMain = "reeot - Installer";
constexpr const char *kTitleRepair = "reeot - Repair";
constexpr const char *kTitleOptions = "reeot - Options";
constexpr const char *kRepairNotice =
    "An existing install was detected. Select the disc image again to add or repair files; files "
    "already installed are left in place.";
constexpr const char *kSpaceHint = "(~6 GB required)";

}

void InitInstallerFonts(ImFontAtlas *atlas) {
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;
  cfg.OversampleH = 2;
  cfg.OversampleV = 2;
  auto load = [&](float px) {
    constexpr auto kFont = eot::Embedded("installer/HelveticaNeueRoman.otf");
    return atlas->AddFontFromMemoryTTF(const_cast<uint8_t *>(kFont.data), static_cast<int>(kFont.size), px,
                                       &cfg);
  };
  g_body_font = load(18.0f);
  g_title_font = load(40.0f);
  g_path_font = load(13.0f);
  if (!g_body_font)
    EOT_WARN("[install] the installer font did not load; the wizard uses the drawer default");
}

InstallerWizard::InstallerWizard(rex::ui::ImGuiDrawer *drawer, rex::ui::ImmediateDrawer *immediate_drawer,
                                 rex::ui::WindowedAppContext &app_context,
                                 const std::filesystem::path &default_install_dir, bool repair,
                                 const InstallConfig *existing, CompletionCallback on_done)
    : ImGuiDialog(drawer), app_context_(app_context), immediate_drawer_(immediate_drawer),
      on_done_(std::move(on_done)), repair_(repair), install_dir_(default_install_dir) {
  if (existing)
    disc_fingerprint_ = existing->disc_fingerprint;
  Prefill();
}

void InstallerWizard::Prefill() {
  const std::string disc = REXCVAR_GET(eot_install_disc);
  if (!disc.empty()) {
    disc_path_ = disc;
    ValidateDisc();
  }
  const std::string update = REXCVAR_GET(eot_install_update);
  if (!update.empty()) {
    update_path_ = update;
    ValidateUpdate();
  }
  const std::string dlc = REXCVAR_GET(eot_install_dlc);
  size_t start = 0;
  while (start <= dlc.size()) {
    const size_t sep = dlc.find(';', start);
    const std::string one = dlc.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
    if (!one.empty())
      AddDlc(one);
    if (sep == std::string::npos)
      break;
    start = sep + 1;
  }
  const std::string dir = REXCVAR_GET(eot_install_dir);
  if (!dir.empty())
    install_dir_ = dir;
  unattended_ = REXCVAR_GET(eot_install_unattended) && !disc.empty();
  if (unattended_)
    EOT_INFO("[install] unattended install from {} into {}", disc, install_dir_.string());
}

void InstallerWizard::AddDlc(const std::filesystem::path &path) {
  for (const auto &d : dlc_)
    if (d.path == path)
      return;
  DlcEntry entry;
  entry.path = path;
  const PackageInfo info = InspectPackage(entry.path);
  const std::string why = info.ok ? CheckDlcPackage(info) : info.error;
  entry.valid = why.empty();
  entry.name = info.display_name.empty() ? entry.path.filename().string() : info.display_name;
  entry.status = entry.valid ? "Valid" : why;
  dlc_.push_back(std::move(entry));
}

InstallerWizard::~InstallerWizard() {
  if (install_thread_.joinable()) {
    progress_.canceled.store(true);
    install_thread_.join();
  }
}

void InstallerWizard::Finish(bool completed) {
  if (finished_)
    return;
  finished_ = true;

  InstallConfig cfg;
  cfg.install_root = std::filesystem::absolute(install_dir_);
  cfg.disc_fingerprint = disc_fingerprint_;

  EOT_INFO("[install] wizard finished, completed={}", completed);

  auto cb = on_done_;
  const WizardChoices choices = choices_;
  app_context_.CallInUIThreadDeferred([cb, completed, cfg, choices]() { cb(completed, cfg, choices); });
}

void InstallerWizard::ValidateDisc() {
  disc_valid_ = false;
  disc_fingerprint_.clear();
  if (disc_path_.empty()) {
    disc_status_.clear();
    return;
  }
  auto disc = OpenDiscImage(disc_path_);
  if (!disc) {
    disc_status_ = "Could not read as Xbox 360 disc image";
    return;
  }
  if (!installer::ValidateDisc(*disc)) {
    disc_status_ = "Not the Edge of Time disc";
    return;
  }
  disc_valid_ = true;
  disc_fingerprint_ = DiscFingerprint(disc_path_, *disc);
  disc_status_ = "Valid";
}

void InstallerWizard::ValidateUpdate() {
  update_valid_ = false;
  if (update_path_.empty()) {
    update_status_.clear();
    return;
  }
  const PackageInfo info = InspectPackage(update_path_);
  const std::string why = info.ok ? CheckUpdatePackage(info) : info.error;
  if (!why.empty()) {
    update_status_ = why;
    return;
  }
  update_valid_ = true;
  update_status_ = "Valid";
}

bool InstallerWizard::InputsReady() const {
  if (!disc_valid_ || install_dir_.empty())
    return false;
  if (!update_path_.empty() && !update_valid_)
    return false;
  for (const auto &d : dlc_)
    if (!d.valid)
      return false;
  return true;
}

void InstallerWizard::PickDisc() {
  const platform::FileFilter kFilters[] = {
      {L"Xbox 360 disc image", L"*.iso"},
      {L"All files", L"*.*"},
  };
  auto picked = platform::ShowOpenFileDialog(L"Select Xbox 360 Disc Image", kFilters);
  if (!picked)
    return;
  disc_path_ = *picked;
  ValidateDisc();
}

void InstallerWizard::PickUpdate() {
  const platform::FileFilter kFilters[] = {
      {L"Title update package", L"*.*"},
  };
  auto picked = platform::ShowOpenFileDialog(L"Select the Title Update Package", kFilters);
  if (!picked)
    return;
  update_path_ = *picked;
  ValidateUpdate();
}

void InstallerWizard::PickDlc() {
  const platform::FileFilter kFilters[] = {
      {L"Content package", L"*.*"},
  };
  auto picked = platform::ShowOpenFileDialog(L"Select a Content Package", kFilters);
  if (!picked)
    return;
  AddDlc(*picked);
}

void InstallerWizard::PickInstallDir() {
  auto picked = platform::ShowOpenFolderDialog(L"Select Install Location");
  if (!picked)
    return;
  install_dir_ = *picked;
  install_status_.clear();
}

void InstallerWizard::StartInstall() {
  progress_.files_done.store(0);
  progress_.files_total.store(0);
  progress_.bytes_done.store(0);
  progress_.bytes_total.store(0);
  progress_.complete.store(false);
  progress_.failed.store(false);
  progress_.canceled.store(false);
  progress_.SetCurrentFile("");
  progress_.SetError("");
  done_message_.clear();
  done_success_ = false;
  install_status_.clear();
  page_ = Page::Installing;

  const auto abs_install = std::filesystem::absolute(install_dir_);
  const auto abs_game = abs_install / "game";
  EOT_INFO("[install] install root -> '{}'", abs_install.string());
  EOT_INFO("[install]   game data  -> '{}'", abs_game.string());

  InstallSources sources;
  sources.disc = disc_path_;
  sources.update = update_path_;
  for (const auto &d : dlc_)
    sources.dlc.push_back(d.path);
  sources.packages = platform::ProgramDir() / "pkz";

  try {
    install_thread_ = Installer::RunAsync(sources, abs_game, repair_, progress_);
  } catch (const std::system_error &e) {
    EOT_ERROR("[install] could not start the install thread: {}", e.what());
    progress_.SetError(std::string("Failed to start install: ") + e.what());
    progress_.failed.store(true);
    progress_.complete.store(true);
  }
}

void InstallerWizard::OnDraw(ImGuiIO &) {
  if (unattended_ && page_ == Page::Content && !finished_) {
    if (InputsReady()) {
      StartInstall();
    } else {
      EOT_ERROR("[install] unattended install cannot start: disc {} update {} dir {}", disc_status_,
                update_status_, install_dir_.string());
      unattended_ = false;
    }
  }
  if (unattended_ && page_ == Page::Done) {
    Finish(done_success_);
  }

  if (!background_texture_ && !background_tried_ && immediate_drawer_) {
    background_tried_ = true;
    int w = 0, h = 0, channels = 0;
    constexpr auto kImage = eot::Embedded("installer/installer.png");
    uint8_t *rgba =
        stbi_load_from_memory(kImage.data, static_cast<int>(kImage.size), &w, &h, &channels, 4);
    if (!rgba) {
      EOT_ERROR("[install] the background image did not decode: {}", stbi_failure_reason());
    } else {
      background_texture_ =
          immediate_drawer_->CreateTexture(static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                           rex::ui::ImmediateTextureFilter::kLinear, false, rgba);
      stbi_image_free(rgba);
      if (!background_texture_)
        EOT_ERROR("[install] the background texture could not be created");
    }
  }

  auto *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowBgAlpha(0.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##installer", nullptr, flags)) {
    if (background_texture_) {
      const ImVec2 p0 = vp->WorkPos;
      const ImVec2 p1(p0.x + vp->WorkSize.x, p0.y + vp->WorkSize.y);
      ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(background_texture_.get()), p0, p1);
    }

    constexpr float kPanelMaxWidth = 1040.0f;
    constexpr float kPanelMinWidth = 420.0f;
    constexpr float kPanelMargin = 32.0f;
    const float panel_width =
        std::clamp(vp->WorkSize.x - kPanelMargin * 2.0f, kPanelMinWidth, kPanelMaxWidth);
    ImGui::SetCursorPos(ImVec2(kPanelMargin, kPanelMargin));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::Theme::kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 7));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 8));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, vp->WorkSize.y - kPanelMargin * 2.0f));
    if (ImGui::BeginChild("##installer_panel", ImVec2(panel_width, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoSavedSettings)) {
      if (g_body_font)
        ImGui::PushFont(g_body_font);
      switch (page_) {
      case Page::Content:
        DrawContent();
        break;
      case Page::Options:
        DrawOptions();
        break;
      case Page::Installing:
        DrawInstalling();
        break;
      case Page::Done:
        DrawDone();
        break;
      }
      if (g_body_font)
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
  }
  ImGui::End();
}

namespace {

void DrawTitle(const char *text) {
  if (g_title_font)
    ImGui::PushFont(g_title_font);
  ImGui::TextUnformatted(text);
  if (g_title_font)
    ImGui::PopFont();
}

void SectionHeader(const char *text) {
  ImGui::TextUnformatted(text);
  ImGui::Separator();
  ImGui::Spacing();
}

void FilenameCell(const std::filesystem::path &path) {
  if (path.empty()) {
    ImGui::TextDisabled("not selected");
    return;
  }
  ImGui::TextUnformatted(path.filename().string().c_str());
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", path.string().c_str());
}

void StatusCell(bool valid, const std::string &status) {
  if (status.empty())
    return;
  const ImVec4 color = valid ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(color, "%s", status.c_str());
}

void DirectoryRow(const char *heading, const char *sublabel, const std::filesystem::path &path, const char *id,
                  const std::function<void()> &on_change) {
  ImGui::PushID(id);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(heading);
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, ui::Theme::White(0.55f));
  ImGui::TextUnformatted(sublabel);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::Button("Change"))
    on_change();

  if (g_path_font)
    ImGui::PushFont(g_path_font);
  ImGui::Indent(12.0f);
  if (path.empty())
    ImGui::TextDisabled("not selected");
  else
    ImGui::TextWrapped("%s", path.string().c_str());
  ImGui::Unindent(12.0f);
  if (g_path_font)
    ImGui::PopFont();
  ImGui::PopID();
}

constexpr float kLabelColumn = 190.0f;
constexpr float kValueWidth = 200.0f;

bool BeginRows(const char *id) {
  if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit))
    return false;
  ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, kLabelColumn);
  ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

void OptionRow(const char *label, int count, int selected, const std::function<const char *(int)> &text,
               const std::function<void(int)> &pick) {
  if (count <= 0)
    return;
  ImGui::PushID(label);
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  const char *current = selected >= 0 && selected < count ? text(selected) : "";
  ImGui::SetNextItemWidth(kValueWidth);
  if (ImGui::BeginCombo("##value", current)) {
    for (int i = 0; i < count; ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(text(i), i == selected))
        pick(i);
      if (i == selected)
        ImGui::SetItemDefaultFocus();
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
}

struct StringRow {
  const char *label;
  const char *cvar;
  std::vector<std::pair<const char *, const char *>> values;
};

int RowSelected(const StringRow &row) {
  const std::string current = rex::cvar::GetFlagByName(row.cvar);
  for (size_t i = 0; i < row.values.size(); ++i)
    if (current == row.values[i].second)
      return static_cast<int>(i);
  return -1;
}

}

void InstallerWizard::DrawContent() {
  DrawTitle(repair_ ? kTitleRepair : kTitleMain);
  ImGui::Spacing();

  if (repair_) {
    ImGui::TextWrapped("%s", kRepairNotice);
    ImGui::Spacing();
  }

  DrawSources();

  ImGui::Dummy(ImVec2(0, 10));
  SectionHeader("Install Directory");
  DirectoryRow("Install Location", repair_ ? "(existing install)" : kSpaceHint, install_dir_, "install_dir",
               [this]() { PickInstallDir(); });

  ImGui::Dummy(ImVec2(0, 10));
  DrawDlcSection();
  DrawFooter();
}

void InstallerWizard::DrawSources() {
  SectionHeader("Install Sources");

  const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody;
  if (!ImGui::BeginTable("##inputs", 3, flags))
    return;
  ImGui::TableSetupColumn("##btn", ImGuiTableColumnFlags_WidthFixed, 170.0f);
  ImGui::TableSetupColumn("##path", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("##status", ImGuiTableColumnFlags_WidthFixed, 260.0f);

  ImGui::PushID("disc");
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  if (ImGui::Button("Select Disc Image", ImVec2(-FLT_MIN, 0)))
    PickDisc();
  ImGui::TableSetColumnIndex(1);
  ImGui::AlignTextToFramePadding();
  FilenameCell(disc_path_);
  ImGui::TableSetColumnIndex(2);
  StatusCell(disc_valid_, disc_status_);
  ImGui::PopID();

  ImGui::PushID("update");
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  if (ImGui::Button("Select Title Update", ImVec2(-FLT_MIN, 0)))
    PickUpdate();
  ImGui::TableSetColumnIndex(1);
  ImGui::AlignTextToFramePadding();
  if (update_path_.empty())
    ImGui::TextDisabled("optional: the game runs without it");
  else
    FilenameCell(update_path_);
  ImGui::TableSetColumnIndex(2);
  StatusCell(update_valid_, update_status_);
  ImGui::PopID();

  ImGui::EndTable();
}

void InstallerWizard::DrawDlcSection() {
  SectionHeader("Downloadable Content");

  if (dlc_.empty()) {
    ImGui::TextDisabled("No content packages selected.");
  } else {
    const char *remove_label = "Remove";
    const float remove_width =
        ImGui::CalcTextSize(remove_label).x + ImGui::GetStyle().FramePadding.x * 2.0f + 16.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 5));
    if (ImGui::BeginTable("##dlc", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody)) {
      ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("##status", ImGuiTableColumnFlags_WidthFixed, 260.0f);
      ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, remove_width);
      for (size_t i = 0; i < dlc_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(dlc_[i].name.c_str());
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", dlc_[i].path.string().c_str());
        ImGui::TableSetColumnIndex(1);
        StatusCell(dlc_[i].valid, dlc_[i].status);
        ImGui::TableSetColumnIndex(2);
        if (ImGui::Button(remove_label, ImVec2(remove_width, 0))) {
          dlc_.erase(dlc_.begin() + static_cast<long>(i));
          ImGui::PopID();
          break;
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
  }

  ImGui::Spacing();
  if (ImGui::Button("Add package...", ImVec2(160, 0)))
    PickDlc();
}

void InstallerWizard::DrawOptions() {
  DrawTitle(kTitleOptions);
  ImGui::Spacing();

  auto pick = [this](const char *cvar, const char *value) {
    rex::cvar::SetFlagByName(cvar, value);
    std::erase_if(choices_.settings, [&](const SettingPick &p) { return p.cvar == cvar; });
    choices_.settings.push_back({cvar, value});
  };
  auto draw_row = [&](const StringRow &row) {
    OptionRow(
        row.label, static_cast<int>(row.values.size()), RowSelected(row),
        [&row](int i) { return row.values[static_cast<size_t>(i)].first; },
        [&row, &pick](int i) { pick(row.cvar, row.values[static_cast<size_t>(i)].second); });
  };

  SectionHeader("Display");
  if (BeginRows("##display_rows")) {
    static const StringRow kDisplayMode{"Display mode", "fullscreen", {{"Windowed", "false"}, {"Fullscreen", "true"}}};
    static const StringRow kResolution{
        "Resolution", "eot_resolution", {{"720p", "720p"}, {"1080p", "1080p"}, {"1440p", "1440p"}, {"2160p (4K)", "2160p"}}};
    static const StringRow kAspect{"Aspect ratio",
                                   "eot_aspect_ratio",
                                   {{"Auto", "auto"}, {"4:3", "4:3"}, {"16:9", "16:9"}, {"16:10", "16:10"}, {"21:9", "21:9"}, {"32:9", "32:9"}}};
    static const StringRow kFps{"Frame rate limit",
                                "eot_fps_limit",
                                {{"30 fps", "30"}, {"60 fps", "60"}, {"120 fps", "120"}, {"Unlimited", "0"}}};
    draw_row(kDisplayMode);
    draw_row(kResolution);
    draw_row(kAspect);
    draw_row(kFps);
    ImGui::EndTable();
  }

  ImGui::Dummy(ImVec2(0, 6));
  SectionHeader("Graphics");
  if (BeginRows("##graphics_rows")) {
    static const StringRow kQuality{"Quality preset",
                                    "eot_quality_preset",
                                    {{"Low", "low"}, {"Medium", "medium"}, {"High", "high"}, {"Custom", "custom"}}};
    draw_row(kQuality);
    ImGui::EndTable();
  }

  ImGui::Dummy(ImVec2(0, 6));
  DrawPreferences();
  DrawFooter();
}

void InstallerWizard::DrawPreferences() {
  SectionHeader("Preferences");
#if defined(_WIN32)
  if (ImGui::Checkbox("Create a desktop shortcut", &create_shortcut_))
    choices_.create_shortcut = create_shortcut_;
#endif
}

void InstallerWizard::DrawFooter() {
  ImGui::Dummy(ImVec2(0, 6));
  ImGui::Separator();
  ImGui::Spacing();

  if (!install_status_.empty()) {
    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "%s", install_status_.c_str());
    ImGui::Spacing();
  }

  constexpr float kButtonWidth = 130.0f;
  constexpr ImVec2 kButton(kButtonWidth, 0);

  if (page_ == Page::Content) {
    if (ImGui::Button("Exit", kButton))
      Finish(false);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - kButtonWidth);
    if (ImGui::Button("Next", kButton))
      page_ = Page::Options;
    return;
  }

  if (ImGui::Button("Back", kButton))
    page_ = Page::Content;
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - kButtonWidth);
  ImGui::BeginDisabled(!InputsReady());
  if (ImGui::Button(repair_ ? "Repair" : "Install", kButton))
    StartInstall();
  ImGui::EndDisabled();
}

void InstallerWizard::DrawInstalling() {
  DrawTitle(repair_ ? "Repairing..." : "Installing...");
  ImGui::Spacing();

  const size_t total_bytes = progress_.bytes_total.load();
  const size_t done_bytes = progress_.bytes_done.load();
  const float fraction =
      total_bytes == 0 ? 0.0f : static_cast<float>(done_bytes) / static_cast<float>(total_bytes);
  ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), nullptr);

  auto format_bytes = [](size_t bytes) {
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    char buf[32];
    const double b = static_cast<double>(bytes);
    if (b >= kGiB)
      std::snprintf(buf, sizeof(buf), "%.2f GiB", b / kGiB);
    else
      std::snprintf(buf, sizeof(buf), "%.1f MiB", b / kMiB);
    return std::string(buf);
  };
  ImGui::Text("%s / %s", format_bytes(done_bytes).c_str(), format_bytes(total_bytes).c_str());

  const std::string current = progress_.GetCurrentFile();
  if (!current.empty())
    ImGui::Text("Installing: %s", current.c_str());

  ImGui::Spacing();
  if (ImGui::Button("Cancel", ImVec2(120, 0)))
    progress_.canceled.store(true);

  if (progress_.complete.load()) {
    if (install_thread_.joinable())
      install_thread_.join();
    if (progress_.canceled.load()) {
      install_status_ = "Previous install was canceled. Review inputs and click Install to resume.";
      page_ = Page::Options;
    } else if (progress_.failed.load()) {
      done_success_ = false;
      done_message_ = "Install failed: " + progress_.GetError();
      page_ = Page::Done;
    } else {
      done_success_ = true;
      done_message_ = repair_ ? "Repair complete." : "Install complete.";
      page_ = Page::Done;
    }
  }
}

void InstallerWizard::DrawDone() {
  DrawTitle(done_success_ ? "Done" : "Stopped");
  ImGui::Spacing();
  ImGui::TextWrapped("%s", done_message_.c_str());
  ImGui::Spacing();
  if (done_success_) {
    if (ImGui::Button("Continue", ImVec2(120, 0)))
      Finish(true);
  } else {
    if (ImGui::Button("Quit", ImVec2(120, 0)))
      Finish(false);
  }
}

}
