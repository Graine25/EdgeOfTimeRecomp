#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_GLAPI3dObj_MaterialAnimPlay);
REX_EXTERN(__imp__eot_GLAPI3dObj_MaterialAnimStop); // (object handle r3, r4, material r5)
REX_EXTERN(__imp__eot_3dObj_MaterialAnimStart);

namespace {
using eot::mem::load;

constexpr uint32_t kFxObject = 0x883C29A8;
constexpr uint32_t kFxBodyMaterial = 0x883C29A8 + 12;
constexpr uint32_t kNone = 0xFFFFFFFFu;

constexpr uint32_t kObjectRenderData = 340;
constexpr uint32_t kRenderDataGeometry = 48;
constexpr uint32_t kGeometryMaterials = 84;
constexpr uint32_t kGeometryMaterialCount = 108;
constexpr uint32_t kMaterialSize = 960;
constexpr uint32_t kMaterialAnims = 20;
constexpr uint32_t kMaterialAnimCount = 24;
constexpr uint32_t kMaterialAnimNames = 28;
constexpr uint32_t kMaterialAnimNameCount = 32;
constexpr uint32_t kMaxMaterials = 32;

bool g_mirrorNextStart = false;
uint32_t g_mirroredObject = 0;
uint32_t g_mirroredMask = 0;

uint32_t AnimName(uint32_t material, uint32_t index) {
  const uint32_t names = load<uint32_t>(material + kMaterialAnimNames);
  const uint32_t count = load<uint16_t>(material + kMaterialAnimNameCount);
  for (uint32_t i = 0; names && i < count; ++i)
    if (load<uint32_t>(names + 8 * i + 4) == index)
      return load<uint32_t>(names + 8 * i);
  return 0;
}

uint32_t AnimIndex(uint32_t material, uint32_t crc) {
  const uint32_t names = load<uint32_t>(material + kMaterialAnimNames);
  const uint32_t count = load<uint16_t>(material + kMaterialAnimNameCount);
  for (uint32_t i = 0; names && i < count; ++i)
    if (load<uint32_t>(names + 8 * i) == crc)
      return load<uint32_t>(names + 8 * i + 4);
  return kNone;
}
}

REX_HOOK_RAW(eot_GLAPI3dObj_MaterialAnimPlay) {
  const uint32_t object = ctx.r3.u32, material = ctx.r5.u32;
  g_mirrorNextStart = material != kNone && object == load<uint32_t>(kFxObject) && material == load<uint32_t>(kFxBodyMaterial);
  __imp__eot_GLAPI3dObj_MaterialAnimPlay(ctx, base);
  g_mirrorNextStart = false;
}

REX_HOOK_RAW(eot_3dObj_MaterialAnimStart) {
  const PPCContext entry = ctx;
  __imp__eot_3dObj_MaterialAnimStart(ctx, base);
  if (!g_mirrorNextStart)
    return;
  g_mirrorNextStart = false;
  const uint32_t object = entry.r3.u32, material = entry.r4.u32, handle = entry.r5.u32;
  const uint32_t data = load<uint32_t>(object + kObjectRenderData);
  const uint32_t geometry = data ? load<uint32_t>(data + kRenderDataGeometry) : 0;
  if (!geometry)
    return;
  const uint32_t count = load<uint32_t>(geometry + kGeometryMaterialCount);
  const uint32_t records = load<uint32_t>(geometry + kGeometryMaterials);
  if (!records || count > kMaxMaterials || material >= count)
    return;
  const uint32_t name = AnimName(records + kMaterialSize * material, handle & 0xFFFF);
  if (!name)
    return;
  g_mirroredObject = load<uint32_t>(kFxObject);
  g_mirroredMask = 0;
  for (uint32_t m = 0; m < count; ++m) {
    if (m == material)
      continue;
    const uint32_t record = records + kMaterialSize * m;
    const uint32_t index = AnimIndex(record, name);
    if (index == kNone || index >= load<uint32_t>(record + kMaterialAnimCount))
      continue;
    const uint32_t anim = load<uint32_t>(load<uint32_t>(record + kMaterialAnims) + 4 * index);
    if (!anim)
      continue;
    PPCContext call = entry;
    call.r4.u32 = m;
    call.r5.u32 = (m << 16) | index;
    call.r6.u32 = anim;
    __imp__eot_3dObj_MaterialAnimStart(call, base);
    g_mirroredMask |= 1u << m;
  }
  if (g_mirroredMask)
    EOT_DEBUG("[suit] body animation {:08x} mirrored from material {} to mask {:#x}", name, material, g_mirroredMask);
}

REX_HOOK_RAW(eot_GLAPI3dObj_MaterialAnimStop) {
  const PPCContext entry = ctx;
  const uint32_t object = entry.r3.u32, material = entry.r5.u32;
  __imp__eot_GLAPI3dObj_MaterialAnimStop(ctx, base);
  if (!g_mirroredMask || material == kNone || object != g_mirroredObject || material != load<uint32_t>(kFxBodyMaterial))
    return;
  for (uint32_t m = 0; m < kMaxMaterials; ++m) {
    if (!(g_mirroredMask & (1u << m)))
      continue;
    PPCContext call = entry;
    call.r5.u32 = m;
    __imp__eot_GLAPI3dObj_MaterialAnimStop(call, base);
  }
  g_mirroredMask = 0;
}
