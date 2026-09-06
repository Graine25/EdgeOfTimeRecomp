#include <cstdint>

#include "core/logging.h"
#include "gamelogic/ui/menu_common.h"

REX_EXTERN(__imp__sub_88232718);
REX_EXTERN(__imp__sub_88232128);
REX_EXTERN(__imp__sub_88232998);
REX_EXTERN(__imp__sub_882305D8);

namespace {

using namespace eot::ui;

constexpr uint32_t kSelectedIndexOff = 40;
constexpr uint32_t kRetailCount = 7;
constexpr uint32_t kExitIndex = 7;
constexpr YesNoLayout kPauseYesNo{76, 152, 160, 184, 80};

bool g_confirm_pending = false;

}

void eot_PauseMenu_NavRightBoundFexit(PPCRegister &r31, PPCCRRegister &cr6, PPCXERRegister &xer) {
  cr6.compare<int32_t>(r31.s32, static_cast<int32_t>(kRetailCount + 1), xer);
}

REX_HOOK_RAW(sub_88232718) {
  const uint32_t desc = ctx.r4.u32;
  __imp__sub_88232718(ctx, base);
  const uint32_t count = desc ? eot::mem::load<uint32_t>(desc + kDescCount) : 0;
  const int slot = count == kRetailCount ? AppendEntry(desc, kHandleExitGame) : -1;
  EOT_INFO("[menu] pause bar {:#x}: count {} -> {}, Exit Game slot {}", desc, count,
           desc ? eot::mem::load<uint32_t>(desc + kDescCount) : 0u, slot);
}

REX_HOOK_RAW(sub_88232128) {
  const uint32_t self = ctx.r3.u32;
  if (self && eot::mem::load<uint32_t>(self + kSelectedIndexOff) == kExitIndex) {
    if (!g_confirm_pending) {
      OpenExitConfirm(ctx, base, self, kPauseYesNo);
      g_confirm_pending = true;
    }
    return;
  }
  __imp__sub_88232128(ctx, base);
}

REX_HOOK_RAW(sub_88232998) {
  if (g_confirm_pending && ExitIfConfirmed(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, kPauseYesNo))
    g_confirm_pending = false;
  __imp__sub_88232998(ctx, base);
}

REX_HOOK_RAW(sub_882305D8) {
  g_confirm_pending = false;
  __imp__sub_882305D8(ctx, base);
}
