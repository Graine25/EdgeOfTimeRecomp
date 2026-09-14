#include <bit>
#include <cstdint>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gamelogic/ui/achievements_page.h"
#include "gamelogic/ui/hud_api.h"
#include "gamelogic/ui/menu_common.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_HUDGalleryOptions_HandleInputEvent); // (this r3, event r4)
REX_EXTERN(__imp__eot_HUDGalleryOptions_PushTabs);

namespace {

using namespace eot::ui;
namespace hud = eot::ui::hud;
namespace achievements = eot::ui::achievements;

constexpr uint32_t kScreenSelected = 48;
constexpr uint32_t kScreenTabCount = 52;

constexpr uint32_t kRetailTabs = 5;
constexpr uint32_t kAchievementsTab = kRetailTabs;

constexpr uint32_t kEvtNavVertical = 8;
constexpr uint32_t kEvtAxis = 4;

constexpr const char *kAchievementsLabel = "REEOT_ACHIEVEMENTS";
constexpr const char *kActLabels[achievements::kActs] = {"REEOT_ACT_1", "REEOT_ACT_2", "REEOT_ACT_3"};
uint32_t g_achievements_handle = 0;
uint32_t g_act_handles[achievements::kActs] = {};
bool g_labels_resolved = false;

uint32_t Label(const PPCContext &ctx, uint8_t *base, const char *name) {
  const uint32_t handle = hud::FindString(ctx, base, eot::ui::NameCrc(name));
  if (!handle)
    EOT_WARN("[gallery] {} is not in any mounted package", name);
  return handle;
}

void PushBar(const PPCContext &ctx, uint8_t *base, uint32_t screen, uint32_t cursor) {
  eot::mem::store<uint32_t>(screen + kScreenSelected, cursor);
  PPCContext call = ctx;
  call.r3.u32 = screen;
  __imp__eot_HUDGalleryOptions_PushTabs(call, base);
}

}

void eot_GalleryTabs_Count(PPCRegister &r3, PPCRegister &r11) {
  if (!r3.u32 || r11.u32 != kRetailTabs)
    return;
  eot::mem::store<uint32_t>(r3.u32 + kScreenTabCount, achievements::IsOpen() ? achievements::kActs : kRetailTabs + 1);
}

void eot_GalleryTabs_Labels(PPCRegister &r6, PPCRegister &r31) {
  const uint32_t desc = r6.u32;
  if (!desc || !g_labels_resolved)
    return;
  const uint32_t count = eot::mem::load<uint32_t>(desc + kDescCount);
  if (achievements::IsOpen() && count == achievements::kActs) {
    for (uint32_t act = 0; act < achievements::kActs; ++act) {
      eot::mem::store<uint32_t>(DescHandleAddr(desc, act), g_act_handles[act]);
      eot::mem::store<uint32_t>(DescPropAddr(desc, act), kPropDefault);
    }
    EOT_DEBUG("[gallery] the bar is the three Acts (descriptor {:#x}, screen {:#x})", desc, r31.u32);
  } else if (count == kRetailTabs + 1 && g_achievements_handle) {
    eot::mem::store<uint32_t>(DescHandleAddr(desc, kAchievementsTab), g_achievements_handle);
    eot::mem::store<uint32_t>(DescPropAddr(desc, kAchievementsTab), kPropDefault);
    EOT_DEBUG("[gallery] tab {} is Achievements (descriptor {:#x}, screen {:#x})", kAchievementsTab, desc, r31.u32);
  }
}

REX_HOOK_RAW(eot_HUDGalleryOptions_PushTabs) {
  if (!g_labels_resolved) {
    g_achievements_handle = Label(ctx, base, kAchievementsLabel);
    for (uint32_t act = 0; act < achievements::kActs; ++act)
      g_act_handles[act] = Label(ctx, base, kActLabels[act]);
    g_labels_resolved = g_achievements_handle && g_act_handles[0] && g_act_handles[1] && g_act_handles[2];
    if (g_labels_resolved)
      EOT_INFO("[gallery] tab labels resolved: Achievements {:#010x}, Acts {:#010x} {:#010x} {:#010x}",
               g_achievements_handle, g_act_handles[0], g_act_handles[1], g_act_handles[2]);
  }
  __imp__eot_HUDGalleryOptions_PushTabs(ctx, base);
}

REX_HOOK_RAW(eot_HUDGalleryOptions_HandleInputEvent) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t event = ctx.r4.u32;
  const uint32_t type = event ? eot::mem::load<uint32_t>(event + kEvtType) : 0;
  if (achievements::IsOpen() && self && event) {
    switch (type) {
    case kEvtNavVertical: {
      const float axis = std::bit_cast<float>(eot::mem::load<uint32_t>(event + kEvtAxis));
      achievements::Move(ctx, base, axis > 0.0f ? 1 : -1);
      ConsumeEvent(event);
      return;
    }
    case kEvtNav:
      __imp__eot_HUDGalleryOptions_HandleInputEvent(ctx, base);
      achievements::SetAct(ctx, base, eot::mem::load<uint32_t>(self + kScreenSelected));
      return;
    case kEvtBack:
      achievements::Close(ctx, base);
      PlayCue(ctx, base, kCueBack);
      PushBar(ctx, base, self, kAchievementsTab);
      ConsumeEvent(event);
      return;
    default:
      ConsumeEvent(event);
      return;
    }
  }
  if (self && event && type == kEvtSelect && eot::mem::load<uint32_t>(self + kScreenSelected) == kAchievementsTab) {
    ConsumeEvent(event);
    EOT_INFO("[gallery] Achievements tab selected (screen {:#x})", self);
    if (achievements::Open(ctx, base)) {
      PlayCue(ctx, base, kCueAccept);
      PushBar(ctx, base, self, 0);
    }
    return;
  }
  __imp__eot_HUDGalleryOptions_HandleInputEvent(ctx, base);
}
