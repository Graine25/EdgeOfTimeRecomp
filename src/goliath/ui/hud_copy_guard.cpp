#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_GLAPIHUD_CopyWnd);
REX_EXTERN(__imp__eot_HUDMgrBC_GetWin);

namespace {

constexpr uint32_t kParentObjectOffset = 80;
constexpr uint32_t kHandleOffset = 60;

uint32_t ResolveWindow(PPCContext &ctx, uint8_t *base, uint32_t handle) {
  const uint32_t saved_r3 = ctx.r3.u32;
  ctx.r3.u32 = handle;
  __imp__eot_HUDMgrBC_GetWin(ctx, base);
  const uint32_t window = ctx.r3.u32;
  ctx.r3.u32 = saved_r3;
  return window;
}

}

REX_HOOK_RAW(eot_GLAPIHUD_CopyWnd) {
  const uint32_t source_handle = ctx.r3.u32;
  const uint32_t source = ResolveWindow(ctx, base, source_handle);
  if (source) {
    const uint32_t parent_object = eot::mem::load<uint32_t>(source + kParentObjectOffset);
    if (parent_object) {
      const uint32_t parent_handle = eot::mem::load<uint32_t>(parent_object + kHandleOffset);
      if (!ResolveWindow(ctx, base, parent_handle)) {
        EOT_WARN("[hud] CopyWnd: window {:#x} parent handle {:#x} no longer resolves; skipping "
                 "the copy (the retail path would write through a null window)",
                 source_handle, parent_handle);
        ctx.r3.s64 = -1;
        return;
      }
    }
  }
  __imp__eot_GLAPIHUD_CopyWnd(ctx, base);
}
