#include <cstdint>

#include "core/logging.h"
#include "gamelogic/ui/hud_api.h"
#include "gamelogic/ui/menu_common.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_HUDOptionsScreen_HandleInputEvent); // (this r3, event r4)
REX_EXTERN(__imp__eot_GameOptionsPopup_OnShow);           // (config r3, shown r4)
REX_EXTERN(__imp__eot_GameOptionsPopup_OnUpdate);         // (config r3, dt f1) -> wants to close
REX_EXTERN(__imp__eot_WindowComponent_Teardown);          // (component r3)
REX_EXTERN(__imp__eot_Input_IsPressed);                   // (pad mask r3, input r4) -> pressed this frame

namespace {

using namespace eot::ui;

constexpr uint32_t kRetailCount = 5;
constexpr uint32_t kGraphicsIndex = 5;
constexpr uint32_t kScreenCursorOff = 84;

namespace cfg {
constexpr uint32_t kVtable = 0;
constexpr uint32_t kPriority = 4;
constexpr uint32_t kPadMask = 8;
constexpr uint32_t kInputDelay = 12;
constexpr uint32_t kOpenTime = 16;
constexpr uint32_t kCloseTime = 20;
constexpr uint32_t kBackdropAlpha = 24;
constexpr uint32_t kFlags = 28;
constexpr uint32_t kTitle = 32;
constexpr uint32_t kTitleCount = 6;
constexpr uint32_t kWindow = 56;
constexpr uint32_t kAuxWindow = 60;
constexpr uint32_t kAuxWindowCount = 3;
constexpr uint32_t kOne = 72;
constexpr uint32_t kPage = 76;
constexpr uint32_t kBody = 84;
constexpr uint32_t kResult = 88;
constexpr uint32_t kSize = 320;

constexpr uint8_t kFlagLayerA = 0x02;
constexpr uint8_t kFlagInputBlock = 0x04;
constexpr uint8_t kFlagLayerB = 0x08;
constexpr uint8_t kFlagOwnedByCaller = 0x20;
constexpr uint8_t kFlagBlackout = 0x40;

constexpr uint32_t kResultAccept = 1;
constexpr uint32_t kResultCancel = 2;
constexpr uint32_t kResultNone = 3;

constexpr uint32_t kGameOptionsVtable = 0x880894DC;
}

constexpr uint32_t kComponentConfigOff = 36;

constexpr uint32_t kInputAccept = 9;
constexpr uint32_t kInputBack = 10;
constexpr uint32_t kInputLogMax = 32;

uint32_t g_config = 0;
uint32_t g_popup_id = 0;
bool g_popup_open = false;
uint32_t g_pressed_mask = 0;

bool Pressed(const PPCContext &ctx, uint8_t *base, uint32_t input) {
  PPCContext call = ctx;
  call.r3.u32 = 1;
  call.r4.u32 = input;
  __imp__eot_Input_IsPressed(call, base);
  return (call.r3.u32 & 0xFF) != 0;
}

void FillConfig(uint32_t c, uint32_t window) {
  for (uint32_t off = 0; off < cfg::kSize; off += 4)
    eot::mem::store<uint32_t>(c + off, 0);
  eot::mem::store<uint32_t>(c + cfg::kVtable, cfg::kGameOptionsVtable);
  eot::mem::store<uint32_t>(c + cfg::kPriority, 0);
  eot::mem::store<uint32_t>(c + cfg::kPadMask, 1);
  eot::mem::store<float>(c + cfg::kInputDelay, 0.0f);
  eot::mem::store<float>(c + cfg::kOpenTime, 0.25f);
  eot::mem::store<float>(c + cfg::kCloseTime, 0.15f);
  eot::mem::store<float>(c + cfg::kBackdropAlpha, 0.65f);
  eot::mem::store<uint8_t>(c + cfg::kFlags, cfg::kFlagOwnedByCaller | cfg::kFlagInputBlock);
  for (uint32_t i = 0; i < cfg::kTitleCount; ++i)
    eot::mem::store<uint32_t>(c + cfg::kTitle + i * 4, 0xFFFFFFFFu);
  eot::mem::store<uint32_t>(c + cfg::kTitle, kHandleGraphicsTitle);
  eot::mem::store<uint32_t>(c + cfg::kWindow, window);
  for (uint32_t i = 0; i < cfg::kAuxWindowCount; ++i)
    eot::mem::store<uint32_t>(c + cfg::kAuxWindow + i * 4, 0xFFFFFFFFu);
  eot::mem::store<uint32_t>(c + cfg::kOne, 1);
  eot::mem::store<uint32_t>(c + cfg::kPage, 0);
  eot::mem::store<uint32_t>(c + cfg::kBody, kHandleGraphicsBody);
  eot::mem::store<uint32_t>(c + cfg::kResult, cfg::kResultNone);
}

void OpenPopup(const PPCContext &ctx, uint8_t *base) {
  if (!g_config)
    g_config = AllocGuest(ctx, base, cfg::kSize);
  const uint32_t window = hud::Find(ctx, base, NameCrc("Reeot_GraphicsPanel"));
  if (!g_config || window == hud::kNoWindow) {
    EOT_WARN("[menu] graphics pop-up: config {:#x} panel {:#x}; not opening", g_config, window);
    return;
  }
  FillConfig(g_config, window);
  PPCContext call = ctx;
  call.r3.u32 = g_config;
  __imp__eot_YesNoWindow_Open(call, base);
  g_popup_id = call.r3.u32;
  g_popup_open = g_popup_id != 0xFFFFFFFFu;
  g_pressed_mask = 0;
  EOT_INFO("[menu] graphics pop-up: config {:#x} panel {:#x} -> id {:#x}", g_config, window, g_popup_id);
}

}

void eot_OptionsBar_AddGraphics(PPCRegister &r6, PPCRegister &r31) {
  const uint32_t desc = r6.u32;
  if (!desc || eot::mem::load<uint32_t>(desc + kDescCount) != kRetailCount)
    return;
  const int slot = AppendEntry(desc, kHandleGraphics);
  EOT_INFO("[menu] options bar {:#x}: Graphics slot {}", desc, slot);
  if (slot < 0)
    return;
  eot::mem::store<uint32_t>(desc + kDescSelected, 0);
  if (r31.u32)
    eot::mem::store<uint32_t>(r31.u32 + kScreenCursorOff, 0);
}

void eot_OptionsBar_NavRightBound6(PPCRegister &r29, PPCCRRegister &cr6, PPCXERRegister &xer) {
  cr6.compare<uint32_t>(r29.u32, kGraphicsIndex, xer);
}

REX_HOOK_RAW(eot_HUDOptionsScreen_HandleInputEvent) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t event = ctx.r4.u32;
  const uint32_t type = event ? eot::mem::load<uint32_t>(event + kEvtType) : 0;
  if (g_popup_open) {
    ConsumeEvent(event);
    return;
  }
  if (type == kEvtSelect && self && eot::mem::load<uint32_t>(self + kScreenCursorOff) == kGraphicsIndex) {
    OpenPopup(ctx, base);
    ConsumeEvent(event);
    return;
  }
  __imp__eot_HUDOptionsScreen_HandleInputEvent(ctx, base);
}

REX_HOOK_RAW(eot_GameOptionsPopup_OnShow) {
  if (ctx.r3.u32 != g_config || !g_config) {
    __imp__eot_GameOptionsPopup_OnShow(ctx, base);
    return;
  }
  EOT_INFO("[menu] graphics pop-up: shown {}", ctx.r4.u32 & 0xFF);
}

REX_HOOK_RAW(eot_GameOptionsPopup_OnUpdate) {
  if (ctx.r3.u32 != g_config || !g_config) {
    __imp__eot_GameOptionsPopup_OnUpdate(ctx, base);
    return;
  }
  uint32_t down = 0;
  for (uint32_t input = 0; input < kInputLogMax; ++input)
    if (Pressed(ctx, base, input))
      down |= 1u << input;
  if (down & ~g_pressed_mask)
    EOT_INFO("[menu] graphics pop-up: inputs pressed {:#010x}", down & ~g_pressed_mask);
  g_pressed_mask = down;
  const bool close = (down & ((1u << kInputAccept) | (1u << kInputBack))) != 0;
  if (close)
    eot::mem::store<uint32_t>(g_config + cfg::kResult, cfg::kResultCancel);
  ctx.r3.u32 = close ? 1 : 0;
}

REX_HOOK_RAW(eot_WindowComponent_Teardown) {
  const uint32_t config = ctx.r3.u32 ? eot::mem::load<uint32_t>(ctx.r3.u32 + kComponentConfigOff) : 0;
  __imp__eot_WindowComponent_Teardown(ctx, base);
  if (config && config == g_config) {
    g_popup_open = false;
    EOT_INFO("[menu] graphics pop-up: closed (id {:#x})", g_popup_id);
  }
}
