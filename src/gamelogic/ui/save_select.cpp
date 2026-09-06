#include <cstdint>

#include "core/logging.h"
#include "gamelogic/ui/hud_api.h"
#include "gamelogic/ui/menu_common.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_HUDSavegameSelect_HandleInputEvent); // (this r3, event r4)
REX_EXTERN(__imp__eot_HUDSavegameSelect_HandleMessage);    // (this r3, message r4, payload r5) -> handled

namespace {

using namespace eot::ui;

constexpr YesNoLayout kSaveYesNo{68, 144, 152, 172, 72};

bool g_confirm_pending = false;

}

REX_HOOK_RAW(eot_HUDSavegameSelect_HandleInputEvent) {
  const uint32_t self = ctx.r3.u32, event = ctx.r4.u32;
  if (self && event && eot::mem::load<uint32_t>(event + kEvtType) == kEvtBack && !g_confirm_pending) {
    ConsumeEvent(event);
    const uint32_t handle = OpenConfirm(ctx, base, self, kSaveYesNo, kHandleLeaveTitle, kHandleLeaveBody,
                                        NameCrc("Reeot_LeaveWindow"), NameCrc("Reeot_LeaveYes"), NameCrc("Reeot_LeaveNo"));
    g_confirm_pending = handle != 0xFFFFFFFFu;
    EOT_INFO("[menu] save selection: B, leave-the-game window {:#x}", handle);
    return;
  }
  __imp__eot_HUDSavegameSelect_HandleInputEvent(ctx, base);
}

REX_HOOK_RAW(eot_HUDSavegameSelect_HandleMessage) {
  if (g_confirm_pending && ExitIfConfirmed(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, kSaveYesNo)) {
    g_confirm_pending = false;
    EOT_INFO("[menu] save selection: leave-the-game window answered");
    ctx.r3.u32 = 1;
    return;
  }
  __imp__eot_HUDSavegameSelect_HandleMessage(ctx, base);
}
