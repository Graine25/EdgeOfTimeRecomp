#include <cstdint>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gamelogic/ui/menu_common.h"

REX_EXTERN(__imp__sub_8823B328);

namespace {

using namespace eot::ui;

constexpr uint32_t kRetailCount = 5;
constexpr uint32_t kGraphicsIndex = 5;
constexpr uint32_t kScreenCursorOff = 84;

bool MenuOpen() { return rex::cvar::Query<bool>(kGraphicsMenuCvar); }
void SetMenuOpen(bool on) { rex::cvar::SetFlagByName(kGraphicsMenuCvar, on ? "true" : "false"); }

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

REX_HOOK_RAW(sub_8823B328) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t event = ctx.r4.u32;
  const uint32_t type = event ? eot::mem::load<uint32_t>(event + kEvtType) : 0;
  if (MenuOpen()) {
    if (type == kEvtBack || type == kEvtCancel)
      SetMenuOpen(false);
    ConsumeEvent(event);
    return;
  }
  if (type == kEvtSelect && self && eot::mem::load<uint32_t>(self + kScreenCursorOff) == kGraphicsIndex) {
    SetMenuOpen(true);
    ConsumeEvent(event);
    return;
  }
  __imp__sub_8823B328(ctx, base);
}
