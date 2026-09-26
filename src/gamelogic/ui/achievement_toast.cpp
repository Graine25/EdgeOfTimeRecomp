#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/hook.h>
#include <rex/ppc/func.h>

#include "core/export.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gamelogic/ui/hud_api.h"
#include "gamelogic/ui/menu_common.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_HUDManager_PostUpdate); // (this r3, ...): the HUD, once a frame

namespace eot::ui::achievements {
namespace {

namespace hud = eot::ui::hud;
using clock = std::chrono::steady_clock;

using TakeFn = int32_t (*)(char *, int32_t, int32_t *, int32_t *);
TakeFn Take() {
  static const TakeFn fn = reinterpret_cast<TakeFn>(HostEntryPoint("eot_ach_toast_take"));
  return fn;
}

constexpr uint32_t kSheetColumns = 8;
constexpr uint32_t kSheetRows = 7;

constexpr float kRestX = 0.6406f;
constexpr float kHiddenX = 1.02f;
constexpr float kSlideIn = 0.30f;
constexpr float kHold = 4.20f;
constexpr float kSlideOut = 0.45f;

constexpr size_t kNameFits = 30;

constexpr float kTextXWithIcon = 0.170f;
constexpr float kTextXAlone = 0.045f;
constexpr float kTextRight = 0.965f;

constexpr uint32_t kTextWndSetScale = 18;
constexpr uint32_t kTextWndGetXYScale = 67;
constexpr float kTitleScale = 0.70f;
constexpr float kNameScale = 0.88f;

constexpr uint32_t kScratchFloats = 0;
constexpr uint32_t kScratchText = 64;
constexpr uint32_t kScratchSize = 192;

enum class Phase : uint8_t { kIdle, kIn, kHold, kOut };

constexpr float kPlateU = 1.0f;
constexpr float kPlateV = 96.0f / 128.0f;

struct Windows {
  uint32_t root = hud::kNoWindow;
  uint32_t plate = hud::kNoWindow;
  uint32_t icon = hud::kNoWindow;
  uint32_t title = hud::kNoWindow;
  uint32_t name = hud::kNoWindow;
  bool found = false;
};
Windows g_windows;
uint32_t g_scratch = 0;
bool g_sized = false;
Phase g_phase = Phase::kIdle;
clock::time_point g_since;
bool g_warned = false;

void WriteFloats(uint32_t address, const float *values, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i)
    eot::mem::store<uint32_t>(address + i * 4, std::bit_cast<uint32_t>(values[i]));
}

void SetRect(const PPCContext &ctx, uint8_t *base, uint32_t window, float x, float y, float w, float h) {
  if (window == hud::kNoWindow)
    return;
  const uint32_t rect = g_scratch + kScratchFloats;
  hud::Call(ctx, base, hud::kWndGetPos, window, rect);
  const float wanted[4] = {x, y, w, h};
  for (uint32_t i = 0; i < 4; ++i)
    if (wanted[i] >= 0.0f)
      eot::mem::store<uint32_t>(rect + i * 4, std::bit_cast<uint32_t>(wanted[i]));
  hud::Call(ctx, base, hud::kWndSetPos, window, rect);
}

void SetUVs(const PPCContext &ctx, uint8_t *base, uint32_t window, float u0, float v0, float u1, float v1) {
  if (window == hud::kNoWindow)
    return;
  const float corners[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
  const uint32_t scratch = g_scratch + kScratchFloats;
  WriteFloats(scratch, corners, 8);
  for (uint32_t wide = 0; wide <= 1; ++wide)
    hud::Call(ctx, base, hud::kWnd2DSetUVs, window, scratch, scratch + 8, scratch + 16, scratch + 24, wide);
}

void SetIcon(const PPCContext &ctx, uint8_t *base, uint32_t image_id) {
  const uint32_t cell = image_id - 1;
  const float u0 = static_cast<float>(cell % kSheetColumns) / kSheetColumns;
  const float v0 = static_cast<float>(cell / kSheetColumns) / kSheetRows;
  SetUVs(ctx, base, g_windows.icon, u0, v0, u0 + 1.0f / kSheetColumns, v0 + 1.0f / kSheetRows);
}

void CallWithFloat(const PPCContext &ctx, uint8_t *base, uint32_t slot, uint32_t window, float value) {
  PPCFunc *fn = hud::Entry(slot) ? rex::runtime::ResolveIndirectFunction(hud::Entry(slot)) : nullptr;
  if (!fn || window == hud::kNoWindow)
    return;
  PPCContext call = ctx;
  call.r3.u32 = window;
  call.f1.f64 = value;
  fn(call, base);
}

void ScaleText(const PPCContext &ctx, uint8_t *base, uint32_t window, float factor) {
  if (window == hud::kNoWindow)
    return;
  const uint32_t scratch = g_scratch + kScratchFloats;
  eot::mem::store<uint32_t>(scratch, 0);
  eot::mem::store<uint32_t>(scratch + 4, 0);
  hud::Call(ctx, base, kTextWndGetXYScale, window, scratch, scratch + 4);
  const float style = std::bit_cast<float>(eot::mem::load<uint32_t>(scratch));
  if (style > 0.0f)
    CallWithFloat(ctx, base, kTextWndSetScale, window, style * factor);
}

void SetLine(const PPCContext &ctx, uint8_t *base, uint32_t window, const char *line) {
  if (window == hud::kNoWindow)
    return;
  const uint32_t text = g_scratch + kScratchText;
  uint32_t n = 0;
  for (uint32_t i = 0; line[i] && n < kScratchSize - kScratchText - 2; ++i) {
    eot::mem::store<uint8_t>(text + n++, static_cast<uint8_t>(line[i]));
    if (line[i] == '%')
      eot::mem::store<uint8_t>(text + n++, static_cast<uint8_t>('%'));
  }
  eot::mem::store<uint8_t>(text + n, 0);
  hud::Call(ctx, base, hud::kTextWndSetString, window, text);
}

bool FindWindows(const PPCContext &ctx, uint8_t *base) {
  if (g_windows.found)
    return true;
  g_windows.root = hud::Find(ctx, base, NameCrc("Reeot_Ach_Toast"));
  g_windows.plate = hud::Find(ctx, base, NameCrc("Reeot_Ach_ToastBg"));
  g_windows.icon = hud::Find(ctx, base, NameCrc("Reeot_Ach_ToastIcon"));
  g_windows.title = hud::Find(ctx, base, NameCrc("Reeot_Ach_ToastTitle"));
  g_windows.name = hud::Find(ctx, base, NameCrc("Reeot_Ach_ToastName"));
  g_windows.found = g_windows.root != hud::kNoWindow;
  if (!g_windows.found && !g_warned) {
    g_warned = true;
    EOT_WARN("[ach] no Reeot_Ach_Toast window; is ReeotAchievements.pkz mounted?");
  }
  return g_windows.found;
}

std::string Fit(const std::string &name) {
  if (name.size() <= kNameFits)
    return name;
  return name.substr(0, kNameFits - 3) + "...";
}

void Show(const PPCContext &ctx, uint8_t *base, const std::string &name, uint32_t image_id) {
  SetUVs(ctx, base, g_windows.plate, 0.0f, 0.0f, kPlateU, kPlateV);
  const bool has_icon = image_id != 0;
  hud::Activate(ctx, base, g_windows.icon, has_icon);
  if (has_icon)
    SetIcon(ctx, base, image_id);
  if (!g_sized) {
    g_sized = true;
    ScaleText(ctx, base, g_windows.title, kTitleScale);
    ScaleText(ctx, base, g_windows.name, kNameScale);
  }
  const float text_x = has_icon ? kTextXWithIcon : kTextXAlone;
  SetRect(ctx, base, g_windows.title, text_x, -1.0f, kTextRight - text_x, -1.0f);
  SetRect(ctx, base, g_windows.name, text_x, -1.0f, kTextRight - text_x, -1.0f);
  SetLine(ctx, base, g_windows.name, Fit(name).c_str());
  SetRect(ctx, base, g_windows.root, kHiddenX, -1.0f, -1.0f, -1.0f);
  hud::Activate(ctx, base, g_windows.root, true);
  g_phase = Phase::kIn;
  g_since = clock::now();
  EOT_INFO("[ach] banner: {} (icon {})", name, image_id);
}

float Ease(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }

void Step(const PPCContext &ctx, uint8_t *base) {
  const float age = std::chrono::duration<float>(clock::now() - g_since).count();
  switch (g_phase) {
  case Phase::kIdle:
    return;
  case Phase::kIn: {
    const float t = std::min(1.0f, age / kSlideIn);
    SetRect(ctx, base, g_windows.root, kHiddenX + (kRestX - kHiddenX) * Ease(t), -1.0f, -1.0f, -1.0f);
    if (t >= 1.0f) {
      g_phase = Phase::kHold;
      g_since = clock::now();
    }
    return;
  }
  case Phase::kHold:
    if (age >= kHold) {
      g_phase = Phase::kOut;
      g_since = clock::now();
    }
    return;
  case Phase::kOut: {
    const float t = std::min(1.0f, age / kSlideOut);
    SetRect(ctx, base, g_windows.root, kRestX + (kHiddenX - kRestX) * Ease(t), -1.0f, -1.0f, -1.0f);
    if (t >= 1.0f) {
      hud::Activate(ctx, base, g_windows.root, false);
      g_phase = Phase::kIdle;
    }
    return;
  }
  }
}

}
}

REX_HOOK_RAW(eot_HUDManager_PostUpdate) {
  __imp__eot_HUDManager_PostUpdate(ctx, base);
  using namespace eot::ui::achievements;
  if (!g_scratch) {
    g_scratch = eot::ui::AllocGuest(ctx, base, kScratchSize);
    if (!g_scratch)
      return;
  }
  if (!FindWindows(ctx, base))
    return;
  if (g_phase == Phase::kIdle) {
    const TakeFn take = Take();
    char name[128] = {};
    int32_t gamerscore = 0, image_id = 0;
    if (take && take(name, sizeof(name), &gamerscore, &image_id))
      Show(ctx, base, name, static_cast<uint32_t>(image_id));
    return;
  }
  Step(ctx, base);
}
