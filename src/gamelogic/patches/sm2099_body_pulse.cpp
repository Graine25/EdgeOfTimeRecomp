#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gamelogic/ui/hud_api.h"

REX_EXTERN(__imp__eot_SM2099_Activate);              // (this r3)
REX_EXTERN(__imp__eot_SM2099HealthFX_HandleMessage); // (this r3, message r4, payload r5)

namespace {
using eot::mem::load;
using eot::mem::store;

constexpr uint32_t kCostumeResolvedMessage = 0x2DE0D24C;

constexpr uint32_t k3dObjApiPtr = 0x883CA1D4;
constexpr uint32_t kFindMaterialIndexFromCRC = 284;
constexpr uint32_t kFindMaterialAnimHandleFromCRC = 356;

constexpr uint32_t kFxTable = 0x883C29A8;
constexpr uint32_t kFxObject = kFxTable + 0;
constexpr uint32_t kFxBodyMaterial = kFxTable + 12;
constexpr uint32_t kFxBodyAnimHurt = kFxTable + 44;
constexpr uint32_t kFxBodyAnimFull = kFxTable + 48;
constexpr uint32_t kFxFlags = 0x883CF960;
constexpr uint8_t kFxFlagOn = 0x08;

constexpr uint32_t kHeroParamBlock = 14596;
constexpr uint32_t kShieldDataOffset = 832;
constexpr uint32_t kRecordBodyMaterial = 6 * 4;
constexpr uint32_t kRecordBodyAnimHurt = 12 * 4;
constexpr uint32_t kRecordBodyAnimFull = 15 * 4;

constexpr uint32_t kNone = 0xFFFFFFFFu;

uint32_t ApiCall(const PPCContext &ctx, uint8_t *base, uint32_t slot, uint32_t r3, uint32_t r4, uint32_t r5,
                 uint32_t r6 = 0) {
  const uint32_t table = load<uint32_t>(k3dObjApiPtr);
  if (!table)
    return kNone;
  return eot::ui::hud::CallAt(ctx, base, load<uint32_t>(table + slot), r3, r4, r5, r6);
}

uint32_t ShieldDataRecord(uint32_t self) {
  const uint32_t block = load<uint32_t>(self + kHeroParamBlock);
  const uint32_t row = block ? load<uint32_t>(block) : 0;
  if (!row)
    return 0;
  return row + kShieldDataOffset + load<uint16_t>(row + kShieldDataOffset);
}

uint32_t g_bodyMaterialCrc = 0;

bool TableIsLive(const PPCContext &ctx, uint8_t *base) {
  const uint32_t object = load<uint32_t>(kFxObject);
  const uint32_t material = load<uint32_t>(kFxBodyMaterial);
  if (!g_bodyMaterialCrc || object == 0 || object == kNone || material == kNone)
    return false;
  return ApiCall(ctx, base, kFindMaterialIndexFromCRC, object, 0, g_bodyMaterialCrc) == material;
}
}

REX_HOOK_RAW(eot_SM2099_Activate) {
  const uint32_t self = ctx.r3.u32;
  __imp__eot_SM2099_Activate(ctx, base);
  const uint32_t object = load<uint32_t>(kFxObject);
  const uint32_t record = ShieldDataRecord(self);
  if (!record || object == 0 || object == kNone)
    return;
  g_bodyMaterialCrc = load<uint32_t>(record + kRecordBodyMaterial);
  if (load<uint32_t>(kFxBodyMaterial) != kNone)
    return;
  const uint32_t material = ApiCall(ctx, base, kFindMaterialIndexFromCRC, object, 0, g_bodyMaterialCrc);
  if (material == kNone) {
    store<uint8_t>(kFxFlags, load<uint8_t>(kFxFlags) & static_cast<uint8_t>(~kFxFlagOn));
    return;
  }
  const uint32_t hurt = ApiCall(ctx, base, kFindMaterialAnimHandleFromCRC, object, 0, material, load<uint32_t>(record + kRecordBodyAnimHurt));
  const uint32_t full = ApiCall(ctx, base, kFindMaterialAnimHandleFromCRC, object, 0, material, load<uint32_t>(record + kRecordBodyAnimFull));
  store<uint32_t>(kFxBodyMaterial, material);
  store<uint32_t>(kFxBodyAnimHurt, hurt);
  store<uint32_t>(kFxBodyAnimFull, full);
  store<uint8_t>(kFxFlags, load<uint8_t>(kFxFlags) | kFxFlagOn);
  EOT_INFO("[suit] 2099 body pulse on a custom suit: object {:#x} material {} animations {:#x} / {:#x}", object, material, full, hurt);
}

REX_HOOK_RAW(eot_SM2099HealthFX_HandleMessage) {
  const uint32_t message = ctx.r4.u32;
  __imp__eot_SM2099HealthFX_HandleMessage(ctx, base);
  if (message == kCostumeResolvedMessage && TableIsLive(ctx, base))
    store<uint8_t>(kFxFlags, load<uint8_t>(kFxFlags) | kFxFlagOn);
}
