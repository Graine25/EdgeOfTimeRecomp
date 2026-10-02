#include "gamelogic/ui/achievements_page.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gamelogic/ui/hud_api.h"
#include "gamelogic/ui/menu_common.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_TextWnd_SetStringHandle); // (const uint32 *window r3, string handle r4)

namespace eot::ui::achievements {
namespace {

namespace hud = eot::ui::hud;

constexpr uint32_t kSheetColumns = 8;
constexpr uint32_t kSheetRows = 6;
constexpr uint32_t kSecretImageId = 48;

constexpr uint32_t kTitleFits = 22;
constexpr uint32_t kTextWndSetScale = 18;
constexpr uint32_t kTextWndGetXYScale = 67;

constexpr uint32_t kRows = 8;
constexpr float kRowY = 0.267f;
constexpr float kRowStep = 0.070f;
constexpr float kKnobH = 0.054f;

constexpr uint32_t kShowWhileLocked = 0x8;

constexpr float kLockedShade = 0.55f;
constexpr float kRowShade = 0.45f;
constexpr float kFullShade = 1.0f;

constexpr uint32_t kScratchFloats = 0;
constexpr uint32_t kScratchHandle = 48;
constexpr uint32_t kScratchText = 64;
constexpr uint32_t kScratchSize = 128;

struct ActOf {
  uint32_t id;
  uint32_t act;
};
constexpr ActOf kActOf[] = {
    {46, 0}, {51, 0}, {55, 0}, {56, 0}, {57, 0}, {61, 0}, {68, 0}, {70, 0}, {73, 0},
    {80, 0}, {81, 0}, {82, 0}, {88, 0}, {91, 0}, {92, 0}, {94, 0}, {95, 0}, {96, 0},
    {47, 1}, {52, 1}, {53, 1}, {58, 1}, {59, 1}, {60, 1}, {62, 1}, {63, 1}, {64, 1},
    {69, 1}, {71, 1}, {74, 1}, {83, 1}, {89, 1},
    {48, 2}, {49, 2}, {50, 2}, {54, 2}, {65, 2}, {66, 2}, {67, 2}, {72, 2}, {75, 2},
    {76, 2}, {77, 2}, {78, 2}, {79, 2}, {84, 2}, {90, 2},
};

uint32_t ActFor(uint32_t id) {
  for (const ActOf &a : kActOf)
    if (a.id == id)
      return a.act;
  return kActs - 1;
}

struct Entry {
  uint32_t id = 0;
  uint32_t image_id = 0;
  uint32_t gamerscore = 0;
  uint32_t name_length = 0;
  uint64_t unlocked_at = 0;
  bool unlocked = false;
  bool secret = false;
};

bool IsHidden(const Entry &entry) { return entry.secret && !entry.unlocked; }

std::vector<Entry> g_entries;
std::vector<uint32_t> g_act_entries[kActs];
uint32_t g_act = 0;
uint32_t g_selected[kActs] = {};
uint32_t g_first[kActs] = {};
uint32_t g_unlocked_count = 0;
uint32_t g_unlocked_score = 0;
uint32_t g_total_score = 0;
uint32_t g_scratch = 0;
bool g_open = false;

struct Windows {
  uint32_t page = hud::kNoWindow;
  uint32_t progress = hud::kNoWindow;
  uint32_t row[kRows] = {};
  uint32_t label[kRows] = {};
  uint32_t icon[kRows] = {};
  uint32_t cursor = hud::kNoWindow;
  uint32_t knob = hud::kNoWindow;
  uint32_t arrow_up = hud::kNoWindow;
  uint32_t arrow_down = hud::kNoWindow;
  uint32_t name = hud::kNoWindow;
  uint32_t description = hud::kNoWindow;
  uint32_t status = hud::kNoWindow;
  uint32_t score = hud::kNoWindow;
  uint32_t portrait_amazing = hud::kNoWindow;
  uint32_t portrait_2099 = hud::kNoWindow;
  bool found = false;
};
Windows g_windows;

const std::vector<uint32_t> &List() { return g_act_entries[g_act]; }
uint32_t &Selected() { return g_selected[g_act]; }
uint32_t &First() { return g_first[g_act]; }

void WriteFloats(uint32_t address, const float *values, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i)
    eot::mem::store<uint32_t>(address + i * 4, std::bit_cast<uint32_t>(values[i]));
}

uint32_t StringFor(const PPCContext &ctx, uint8_t *base, const char *name) {
  const uint32_t handle = hud::FindString(ctx, base, eot::ui::NameCrc(name));
  if (!handle)
    EOT_WARN("[ach] no string named {}; is ReeotAchievements.pkz mounted?", name);
  return handle;
}

uint32_t StringFor(const PPCContext &ctx, uint8_t *base, uint32_t id, const char *suffix) {
  char name[48];
  std::snprintf(name, sizeof(name), "REEOT_ACH_%u_%s", id, suffix);
  return StringFor(ctx, base, name);
}

void SetStringHandle(const PPCContext &ctx, uint8_t *base, uint32_t window, uint32_t string_handle) {
  if (window == hud::kNoWindow || !window || !string_handle)
    return;
  eot::mem::store<uint32_t>(g_scratch + kScratchHandle, window);
  PPCContext call = ctx;
  call.r3.u32 = g_scratch + kScratchHandle;
  call.r4.u32 = string_handle;
  __imp__eot_TextWnd_SetStringHandle(call, base);
}

void SetLine(const PPCContext &ctx, uint8_t *base, uint32_t window, const char *line) {
  if (window == hud::kNoWindow || !window)
    return;
  const uint32_t text = g_scratch + kScratchText;
  uint32_t n = 0;
  for (uint32_t i = 0; line[i] && n < 62; ++i) {
    eot::mem::store<uint8_t>(text + n++, static_cast<uint8_t>(line[i]));
    if (line[i] == '%')
      eot::mem::store<uint8_t>(text + n++, static_cast<uint8_t>('%'));
  }
  eot::mem::store<uint8_t>(text + n, 0);
  hud::Call(ctx, base, hud::kTextWndSetString, window, text);
}

void SetUVs(const PPCContext &ctx, uint8_t *base, uint32_t window, const float corners[8]) {
  if (window == hud::kNoWindow)
    return;
  const uint32_t scratch = g_scratch + kScratchFloats;
  WriteFloats(scratch, corners, 8);
  for (uint32_t wide = 0; wide <= 1; ++wide)
    hud::Call(ctx, base, hud::kWnd2DSetUVs, window, scratch, scratch + 8, scratch + 16, scratch + 24, wide);
}

void SetIcon(const PPCContext &ctx, uint8_t *base, uint32_t window, uint32_t image_id) {
  if (!image_id) {
    hud::Activate(ctx, base, window, false);
    return;
  }
  hud::Activate(ctx, base, window, true);
  const uint32_t cell = image_id - 1;
  const float u0 = static_cast<float>(cell % kSheetColumns) / kSheetColumns;
  const float v0 = static_cast<float>(cell / kSheetColumns) / kSheetRows;
  const float u1 = u0 + 1.0f / kSheetColumns;
  const float v1 = v0 + 1.0f / kSheetRows;
  const float corners[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
  SetUVs(ctx, base, window, corners);
}

void SetShade(const PPCContext &ctx, uint8_t *base, uint32_t window, float shade) {
  if (window == hud::kNoWindow)
    return;
  const uint32_t scratch = g_scratch + kScratchFloats;
  const float colour[4] = {shade, shade, shade, 1.0f};
  WriteFloats(scratch, colour, 4);
  hud::Call(ctx, base, hud::kWndSetColors, window, scratch, scratch, scratch, scratch);
}

float g_title_style_scale = 0.0f;

void FitTitle(const PPCContext &ctx, uint8_t *base, uint32_t length) {
  if (g_windows.name == hud::kNoWindow)
    return;
  PPCFunc *set = rex::runtime::ResolveIndirectFunction(hud::Entry(kTextWndSetScale));
  if (!set)
    return;
  if (g_title_style_scale <= 0.0f) {
    const uint32_t scratch = g_scratch + kScratchFloats;
    eot::mem::store<uint32_t>(scratch, 0);
    eot::mem::store<uint32_t>(scratch + 4, 0);
    hud::Call(ctx, base, kTextWndGetXYScale, g_windows.name, scratch, scratch + 4);
    g_title_style_scale = std::bit_cast<float>(eot::mem::load<uint32_t>(scratch));
    if (g_title_style_scale <= 0.0f)
      return;
  }
  const float factor = length > kTitleFits ? static_cast<float>(kTitleFits) / static_cast<float>(length) : 1.0f;
  PPCContext call = ctx;
  call.r3.u32 = g_windows.name;
  call.f1.f64 = factor < 1.0f ? g_title_style_scale * factor : 0.0f;
  set(call, base);
}

void SetRect(const PPCContext &ctx, uint8_t *base, uint32_t window, float x, float y, float w, float h) {
  if (window == hud::kNoWindow || !window)
    return;
  const uint32_t rect = g_scratch + kScratchFloats;
  hud::Call(ctx, base, hud::kWndGetPos, window, rect);
  const float wanted[4] = {x, y, w, h};
  for (uint32_t i = 0; i < 4; ++i)
    if (wanted[i] >= 0.0f)
      eot::mem::store<uint32_t>(rect + i * 4, std::bit_cast<uint32_t>(wanted[i]));
  hud::Call(ctx, base, hud::kWndSetPos, window, rect);
}

void DressWindows(const PPCContext &ctx, uint8_t *base) {
  const float turned[8] = {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
  SetUVs(ctx, base, g_windows.arrow_down, turned);
}

void ShowProgress(const PPCContext &ctx, uint8_t *base) {
  char line[64];
  std::snprintf(line, sizeof(line), "%u / %zu    %u / %u G", g_unlocked_count, g_entries.size(), g_unlocked_score,
                g_total_score);
  SetLine(ctx, base, g_windows.progress, line);
}

void ShowStatus(const PPCContext &ctx, uint8_t *base, const Entry &entry) {
  static constexpr const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char line[64] = "Locked";
  if (entry.unlocked)
    std::snprintf(line, sizeof(line), "Unlocked");
  if (entry.unlocked && entry.unlocked_at) {
    const std::chrono::sys_seconds when{std::chrono::seconds{entry.unlocked_at / 10000000ULL - 11644473600ULL}};
    const std::chrono::year_month_day date{std::chrono::floor<std::chrono::days>(when)};
    std::snprintf(line, sizeof(line), "Unlocked %u %s %d", static_cast<uint32_t>(date.day()),
                  kMonths[static_cast<uint32_t>(date.month()) - 1], static_cast<int>(date.year()));
  }
  SetLine(ctx, base, g_windows.status, line);
}

void LoadCatalogue() {
  g_entries.clear();
  for (auto &list : g_act_entries)
    list.clear();
  g_unlocked_count = 0;
  g_unlocked_score = 0;
  g_total_score = 0;
  auto *kernel = rex::system::kernel_state();
  if (!kernel) {
    EOT_WARN("[ach] no kernel state; the page has nothing to show");
    return;
  }
  auto &manager = kernel->achievements();
  uint32_t secrets = 0;
  for (const rex::system::AchievementInfo &info : manager.ListAchievements()) {
    Entry entry;
    entry.id = info.id;
    entry.image_id = info.image_id;
    entry.gamerscore = info.gamerscore;
    entry.name_length = static_cast<uint32_t>(info.label.size());
    entry.unlocked = manager.IsUnlocked(info.id);
    entry.unlocked_at = entry.unlocked ? manager.GetUnlockTime(info.id) : 0;
    entry.secret = (info.flags & kShowWhileLocked) == 0;
    secrets += entry.secret ? 1 : 0;
    g_total_score += info.gamerscore;
    if (entry.unlocked) {
      ++g_unlocked_count;
      g_unlocked_score += info.gamerscore;
    }
    g_act_entries[ActFor(entry.id)].push_back(static_cast<uint32_t>(g_entries.size()));
    g_entries.push_back(entry);
  }
  EOT_INFO("[ach] {} achievements ({} secret) in tabs of {}/{}/{}, {} unlocked, {} of {} G", g_entries.size(),
           secrets, g_act_entries[0].size(), g_act_entries[1].size(), g_act_entries[2].size(), g_unlocked_count,
           g_unlocked_score, g_total_score);
}

bool FindWindows(const PPCContext &ctx, uint8_t *base) {
  if (g_windows.found)
    return true;
  const auto find = [&](const char *name) { return hud::Find(ctx, base, eot::ui::NameCrc(name)); };
  g_windows.page = find("Reeot_Ach_Page");
  g_windows.progress = find("Reeot_Ach_Progress");
  g_windows.cursor = find("Reeot_Ach_Cursor");
  g_windows.knob = find("Reeot_Ach_SliderKnob");
  g_windows.arrow_up = find("Reeot_Ach_ArrowUp");
  g_windows.arrow_down = find("Reeot_Ach_ArrowDown");
  g_windows.name = find("Reeot_Ach_Name");
  g_windows.description = find("Reeot_Ach_Desc");
  g_windows.status = find("Reeot_Ach_Status");
  g_windows.score = find("Reeot_Ach_Score");
  g_windows.portrait_amazing = find("Reeot_Ach_PortraitAmazing");
  g_windows.portrait_2099 = find("Reeot_Ach_Portrait2099");
  for (uint32_t i = 0; i < kRows; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "Reeot_Ach_Row%02u", i);
    g_windows.row[i] = find(name);
    std::snprintf(name, sizeof(name), "Reeot_Ach_Label%02u", i);
    g_windows.label[i] = find(name);
    std::snprintf(name, sizeof(name), "Reeot_Ach_Icon%02u", i);
    g_windows.icon[i] = find(name);
  }
  g_windows.found = g_windows.page != hud::kNoWindow;
  if (!g_windows.found)
    EOT_WARN("[ach] Reeot_Ach_Page is not there; is ReeotAchievements.pkz mounted?");
  return g_windows.found;
}

void ShowSelection(const PPCContext &ctx, uint8_t *base) {
  const std::vector<uint32_t> &list = List();
  const uint32_t count = static_cast<uint32_t>(list.size());
  if (!count) {
    for (uint32_t row = 0; row < kRows; ++row) {
      hud::Activate(ctx, base, g_windows.row[row], false);
      hud::Activate(ctx, base, g_windows.label[row], false);
      hud::Activate(ctx, base, g_windows.icon[row], false);
    }
    hud::Activate(ctx, base, g_windows.cursor, false);
    return;
  }
  uint32_t &selected = Selected();
  uint32_t &first = First();
  selected = std::min(selected, count - 1);
  if (selected < first)
    first = selected;
  else if (selected >= first + kRows)
    first = selected - kRows + 1;
  if (first + kRows > count)
    first = count > kRows ? count - kRows : 0;

  for (uint32_t row = 0; row < kRows; ++row) {
    const uint32_t index = first + row;
    const bool used = index < count;
    hud::Activate(ctx, base, g_windows.row[row], used);
    hud::Activate(ctx, base, g_windows.label[row], used);
    hud::Activate(ctx, base, g_windows.icon[row], used);
    if (!used)
      continue;
    const Entry &shown = g_entries[list[index]];
    const bool hidden = IsHidden(shown);
    SetStringHandle(ctx, base, g_windows.label[row],
                    hidden ? StringFor(ctx, base, "REEOT_ACH_SECRET_NAME") : StringFor(ctx, base, shown.id, "NAME"));
    SetIcon(ctx, base, g_windows.icon[row], hidden ? kSecretImageId : shown.image_id);
    SetShade(ctx, base, g_windows.icon[row], shown.unlocked ? kFullShade : kLockedShade);
    SetShade(ctx, base, g_windows.row[row], index == selected ? kFullShade : kRowShade);
  }
  SetRect(ctx, base, g_windows.cursor, -1.0f, kRowY + kRowStep * static_cast<float>(selected - first), -1.0f, -1.0f);
  hud::Activate(ctx, base, g_windows.cursor, true);
  const bool scrolls = count > kRows;
  hud::Activate(ctx, base, g_windows.arrow_up, scrolls && first > 0);
  hud::Activate(ctx, base, g_windows.arrow_down, scrolls && first + kRows < count);
  const float travel = scrolls ? static_cast<float>(first) / static_cast<float>(count - kRows) : 0.0f;
  SetRect(ctx, base, g_windows.knob, -1.0f, travel * (1.0f - kKnobH), -1.0f, -1.0f);

  const Entry &entry = g_entries[list[selected]];
  const bool hidden = IsHidden(entry);
  FitTitle(ctx, base, hidden ? 7 : entry.name_length);
  SetStringHandle(ctx, base, g_windows.name,
                  hidden ? StringFor(ctx, base, "REEOT_ACH_SECRET_NAME") : StringFor(ctx, base, entry.id, "NAME"));
  SetStringHandle(ctx, base, g_windows.description,
                  hidden ? StringFor(ctx, base, "REEOT_ACH_SECRET_DESC")
                         : StringFor(ctx, base, entry.id, entry.unlocked ? "DESC" : "LOCKED"));
  SetStringHandle(ctx, base, g_windows.score, StringFor(ctx, base, entry.id, "SCORE"));
  ShowStatus(ctx, base, entry);
}

void ShowPortrait(const PPCContext &ctx, uint8_t *base) {
  hud::Activate(ctx, base, g_windows.portrait_amazing, g_act != 1);
  hud::Activate(ctx, base, g_windows.portrait_2099, g_act == 1);
}

}

bool Open(const PPCContext &ctx, uint8_t *base) {
  if (!g_scratch)
    g_scratch = AllocGuest(ctx, base, kScratchSize);
  LoadCatalogue();
  if (!g_scratch || !FindWindows(ctx, base) || g_entries.empty())
    return false;
  g_act = 0;
  for (uint32_t act = 0; act < kActs; ++act) {
    g_selected[act] = 0;
    g_first[act] = 0;
  }
  DressWindows(ctx, base);
  ShowPortrait(ctx, base);
  ShowSelection(ctx, base);
  ShowProgress(ctx, base);
  hud::Activate(ctx, base, g_windows.page, true);
  g_open = true;
  EOT_INFO("[ach] page open: Act 1, {} of {} rows", std::min<size_t>(kRows, List().size()), List().size());
  return true;
}

void Close(const PPCContext &ctx, uint8_t *base) {
  if (!g_open)
    return;
  hud::Activate(ctx, base, g_windows.page, false);
  g_open = false;
  EOT_INFO("[ach] page closed");
}

bool IsOpen() { return g_open; }

void SetAct(const PPCContext &ctx, uint8_t *base, uint32_t act) {
  if (!g_open || act >= kActs || act == g_act)
    return;
  g_act = act;
  ShowPortrait(ctx, base);
  ShowSelection(ctx, base);
  EOT_INFO("[ach] Act {}: {} achievements", act + 1, List().size());
}

uint32_t Act() { return g_act; }

void Move(const PPCContext &ctx, uint8_t *base, int step) {
  if (!g_open || List().empty())
    return;
  const int next = static_cast<int>(Selected()) + step;
  if (next < 0 || next >= static_cast<int>(List().size()))
    return;
  Selected() = static_cast<uint32_t>(next);
  PlayCue(ctx, base, step > 0 ? kCueDown : kCueUp);
  ShowSelection(ctx, base);
}

}
