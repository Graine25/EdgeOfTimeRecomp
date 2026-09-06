#include <cstdint>

#include "core/logging.h"
#include "gamelogic/ui/hud_api.h"
#include "gamelogic/ui/menu_common.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_HUDOptionsScreen_HandleInputEvent); // (this r3, event r4)

namespace {

using namespace eot::ui;

constexpr uint32_t kRetailCount = 5;
constexpr uint32_t kGraphicsIndex = 5;
constexpr uint32_t kScreenCursorOff = 84;

constexpr const char *kPageWindows[] = {"Reeot_GraphicsBox", "Reeot_GraphicsTitle", "Reeot_GraphicsBody"};

bool g_page_open = false;

void ShowPage(const PPCContext &ctx, uint8_t *base, bool on) {
  for (const char *name : kPageWindows) {
    const uint32_t handle = hud::Find(ctx, base, NameCrc(name));
    hud::Activate(ctx, base, handle, on);
    EOT_DEBUG("[menu] graphics page: {} -> window {:#x} flags {:#x}", name, handle, hud::Flags(ctx, base, handle));
  }
  g_page_open = on;
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
  if (g_page_open) {
    if (type == kEvtBack || type == kEvtCancel)
      ShowPage(ctx, base, false);
    ConsumeEvent(event);
    return;
  }
  if (type == kEvtSelect && self && eot::mem::load<uint32_t>(self + kScreenCursorOff) == kGraphicsIndex) {
    ShowPage(ctx, base, true);
    ConsumeEvent(event);
    return;
  }
  __imp__eot_HUDOptionsScreen_HandleInputEvent(ctx, base);
}
