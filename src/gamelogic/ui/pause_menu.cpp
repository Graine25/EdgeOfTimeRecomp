#include <cstdint>

#include "core/logging.h"
#include "gamelogic/ui/menu_common.h"

REX_EXTERN(__imp__eot_PauseMenu_GetMainMenuBarInfo);
REX_EXTERN(__imp__eot_PauseMenu_HandleMainMenuSelectOption);
REX_EXTERN(__imp__eot_PauseMenu_HandleMessage);
REX_EXTERN(__imp__eot_PauseMenu_EnterOpening);

namespace {

using namespace eot::ui;

constexpr uint32_t kSelectedIndexOff = 40;
constexpr uint32_t kRetailCount = 7;
constexpr uint32_t kQuitGameIndex = 6;
constexpr uint32_t kExitIndex = 7;
constexpr YesNoLayout kPauseYesNo{76, 152, 160, 184, 80};

bool g_confirm_pending = false;

}

void eot_PauseMenu_NavRightBoundFexit(PPCRegister &r31, PPCCRRegister &cr6, PPCXERRegister &xer) {
  cr6.compare<int32_t>(r31.s32, static_cast<int32_t>(kRetailCount + 1), xer);
}

REX_HOOK_RAW(eot_PauseMenu_GetMainMenuBarInfo) {
  const uint32_t desc = ctx.r4.u32;
  __imp__eot_PauseMenu_GetMainMenuBarInfo(ctx, base);
  const uint32_t count = desc ? eot::mem::load<uint32_t>(desc + kDescCount) : 0;
  int slot = -1;
  if (count == kRetailCount) {
    eot::mem::store<uint32_t>(DescHandleAddr(desc, kQuitGameIndex), kHandleExitToMenu);
    slot = AppendEntry(desc, kHandleExitGame);
  }
  EOT_INFO("[menu] pause bar {:#x}: count {} -> {}, Exit Game slot {}", desc, count,
           desc ? eot::mem::load<uint32_t>(desc + kDescCount) : 0u, slot);
}

REX_HOOK_RAW(eot_PauseMenu_HandleMainMenuSelectOption) {
  const uint32_t self = ctx.r3.u32;
  if (self && eot::mem::load<uint32_t>(self + kSelectedIndexOff) == kExitIndex) {
    if (!g_confirm_pending) {
      OpenExitConfirm(ctx, base, self, kPauseYesNo);
      g_confirm_pending = true;
    }
    return;
  }
  __imp__eot_PauseMenu_HandleMainMenuSelectOption(ctx, base);
}

REX_HOOK_RAW(eot_PauseMenu_HandleMessage) {
  if (g_confirm_pending && ExitIfConfirmed(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, kPauseYesNo))
    g_confirm_pending = false;
  __imp__eot_PauseMenu_HandleMessage(ctx, base);
}

REX_HOOK_RAW(eot_PauseMenu_EnterOpening) {
  g_confirm_pending = false;
  __imp__eot_PauseMenu_EnterOpening(ctx, base);
}
