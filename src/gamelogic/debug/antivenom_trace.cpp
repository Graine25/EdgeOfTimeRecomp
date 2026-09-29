#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_BaseAI_SetNextState);             // (object r3, state r4, arg r5)
REX_EXTERN(__imp__eot_BaseAI_PlayAnimSet);
REX_EXTERN(__imp__eot_BaseAI_ActivateAttackCol);        // (object r3, on r4, primitive CRC r5)
REX_EXTERN(__imp__eot_BaseAntiVenom_EnableChargeColPrim); // (on r3), on the object being updated
REX_EXTERN(__imp__eot_BaseHeroSM_EnterHurt);            // (hero r3, kind r4)

namespace {
using eot::mem::load;

constexpr uint32_t kAntiVenomVtables[] = {0x8808C5E0, 0x8808C708, 0x8808C348, 0x8808C498};
constexpr uint32_t kAnimTable = 888;
constexpr uint32_t kAnimCount = 892;

bool IsAntiVenom(uint32_t object) {
  if (!object)
    return false;
  const uint32_t vtable = load<uint32_t>(object);
  for (uint32_t v : kAntiVenomVtables)
    if (vtable == v)
      return true;
  return false;
}

uint32_t AnimHandle(uint32_t object, uint32_t index) {
  if (index >= load<uint32_t>(object + kAnimCount))
    return 0xFFFFFFFFu;
  return load<uint32_t>(load<uint32_t>(object + kAnimTable) + 4 * index);
}
}

REX_HOOK_RAW(eot_BaseAI_SetNextState) {
  const uint32_t object = ctx.r3.u32;
  if (IsAntiVenom(object))
    EOT_INFO("[av] {:#x} state -> {} (arg {:#x})", object, ctx.r4.u32, ctx.r5.u32);
  __imp__eot_BaseAI_SetNextState(ctx, base);
}

REX_HOOK_RAW(eot_BaseAI_PlayAnimSet) {
  const uint32_t object = ctx.r3.u32;
  if (IsAntiVenom(object))
    EOT_INFO("[av] {:#x} anim {} (handle {:#x}, flags {}, blend {:.2f}, speed {:.2f}, start {:.2f})", object,
             ctx.r4.u32, AnimHandle(object, ctx.r4.u32), ctx.r5.u32, ctx.f1.f64, ctx.f2.f64, ctx.f3.f64);
  __imp__eot_BaseAI_PlayAnimSet(ctx, base);
}

REX_HOOK_RAW(eot_BaseAI_ActivateAttackCol) {
  const uint32_t object = ctx.r3.u32;
  if (IsAntiVenom(object))
    EOT_INFO("[av] {:#x} attack collision {} (primitive {:#010x})", object, (ctx.r4.u32 & 0xFF) ? "ON" : "off",
             ctx.r5.u32);
  __imp__eot_BaseAI_ActivateAttackCol(ctx, base);
}

REX_HOOK_RAW(eot_BaseAntiVenom_EnableChargeColPrim) {
  EOT_INFO("[av] charge collision {}", (ctx.r3.u32 & 0xFF) ? "ON" : "off");
  __imp__eot_BaseAntiVenom_EnableChargeColPrim(ctx, base);
}

REX_HOOK_RAW(eot_BaseHeroSM_EnterHurt) {
  EOT_INFO("[av] hero {:#x} hurt (kind {})", ctx.r3.u32, ctx.r4.u32);
  __imp__eot_BaseHeroSM_EnterHurt(ctx, base);
}
