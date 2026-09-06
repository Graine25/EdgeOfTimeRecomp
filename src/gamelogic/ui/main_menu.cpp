#include <cstdint>

#include "core/logging.h"
#include "gamelogic/ui/menu_common.h"

REX_EXTERN(__imp__sub_88351C60); // front input handler (this r3, event r4)
REX_EXTERN(__imp__sub_88352888);
REX_EXTERN(__imp__sub_883519F0); // canonical slot -> displayed index (r3 -> r3)

namespace {

using namespace eot::ui;

constexpr uint32_t kSelectedSlotOff = 100;
constexpr YesNoLayout kFrontYesNo{320, 396, 404, 424, 0};

uint32_t g_appended = 0;
int g_exit_slot = -1;
bool g_confirm_pending = false;

uint32_t DisplayIndexOf(const PPCContext &ctx, uint8_t *base, uint32_t canonical) {
  PPCContext call = ctx;
  call.r3.u32 = canonical;
  __imp__sub_883519F0(call, base);
  return call.r3.u32;
}

void PlaceExitEntry(uint32_t desc) {
  if (!desc)
    return;
  g_appended = 0;
  g_exit_slot = -1;
  const uint32_t before = eot::mem::load<uint32_t>(desc + kDescCount);
  int slot = -1;
  for (uint32_t i = 0; i < before && i < kDescMaxSlots; ++i) {
    const uint32_t h = eot::mem::load<uint32_t>(DescHandleAddr(desc, i));
    if (h == kHandleVipUnlockCode || h == kHandleExitGame) {
      eot::mem::store<uint32_t>(DescHandleAddr(desc, i), kHandleExitGame);
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot < 0) {
    slot = AppendEntry(desc, kHandleExitGame);
    if (slot >= 0)
      g_appended = 1;
  }
  g_exit_slot = slot;
  EOT_INFO("[menu] front bar {:#x}: count {} -> {}, Exit Game at displayed slot {}{}", desc, before,
           eot::mem::load<uint32_t>(desc + kDescCount), slot,
           g_appended ? " (appended)" : " (VIP entry)");
}

}

void eot_FrontMenu_AddEntries(PPCRegister &r6, PPCRegister &) { PlaceExitEntry(r6.u32); }
void eot_FrontMenu_AddEntriesInit(PPCRegister &r6, PPCRegister &) { PlaceExitEntry(r6.u32); }
void eot_FrontMenu_NavRightBound(PPCRegister &r11) { r11.u32 += g_appended; }

REX_HOOK_RAW(sub_88351C60) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t event = ctx.r4.u32;
  const uint32_t type = event ? eot::mem::load<uint32_t>(event + kEvtType) : 0;
  if (type == kEvtSelect && self && g_exit_slot >= 0 &&
      DisplayIndexOf(ctx, base, eot::mem::load<uint32_t>(self + kSelectedSlotOff)) ==
          static_cast<uint32_t>(g_exit_slot)) {
    if (!g_confirm_pending) {
      OpenExitConfirm(ctx, base, self, kFrontYesNo);
      g_confirm_pending = true;
    }
    if (event)
      eot::mem::store<uint8_t>(event + kEvtConsumed, 1);
    return;
  }
  __imp__sub_88351C60(ctx, base);
}

REX_HOOK_RAW(sub_88352888) {
  if (g_confirm_pending && ExitIfConfirmed(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, kFrontYesNo))
    g_confirm_pending = false;
  __imp__sub_88352888(ctx, base);
}
