#include <cstdint>
#include <string>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/pipeline/pipeline_cache.h"

REX_EXTERN(__imp__eot_GLAPIEngine_SetIsInBlockingLoadingScreen);
REX_EXTERN(__imp__eot_GLAPIPackage_Load);
REX_EXTERN(__imp__eot_GLAPIPackage_IsLoaded);
REX_EXTERN(__imp__eot_GLAPIPackage_IsLoading);

namespace {

constexpr uint32_t kPackageMgr = 0x824C8DE8;
constexpr uint32_t kMgrRecords = 16392;
constexpr uint32_t kMaxPackages = 0x1000;

std::string PackageName(uint32_t id) {
  if (id >= kMaxPackages)
    return "?";
  const uint32_t record = eot::mem::load<uint32_t>(kPackageMgr + kMgrRecords + id * 4);
  std::string name;
  for (uint32_t i = 0; record && i < 127; ++i) {
    const char c = static_cast<char>(eot::mem::load<uint8_t>(record + i));
    if (!c)
      break;
    name.push_back(c);
  }
  return name.empty() ? "?" : name;
}

}

REX_HOOK_RAW(eot_GLAPIEngine_SetIsInBlockingLoadingScreen) {
  const bool on = (ctx.r3.u32 & 0xFF) != 0;
  __imp__eot_GLAPIEngine_SetIsInBlockingLoadingScreen(ctx, base);
  eot::gpu::PsoCacheSetLoadingScreen(on);
}

REX_HOOK_RAW(eot_GLAPIPackage_Load) {
  const uint32_t id = ctx.r3.u32;
  __imp__eot_GLAPIPackage_Load(ctx, base);
  if (id == 0 || id >= kMaxPackages)
    return;
  EOT_INFO("[pso] GLAPIPackage::Load({:#x} '{}')", id, PackageName(id));
  eot::gpu::PsoCacheOnPackageLoad(id);
}

REX_HOOK_RAW(eot_GLAPIPackage_IsLoaded) {
  const uint32_t id = ctx.r3.u32;
  __imp__eot_GLAPIPackage_IsLoaded(ctx, base);
  if (ctx.r3.u32 != 0 && id < kMaxPackages && eot::gpu::PsoCacheHoldPackage(id))
    ctx.r3.u32 = 0;
}

REX_HOOK_RAW(eot_GLAPIPackage_IsLoading) {
  const uint32_t id = ctx.r3.u32;
  __imp__eot_GLAPIPackage_IsLoading(ctx, base);
  if (ctx.r3.u32 == 0 && id < kMaxPackages && eot::gpu::PsoCacheHoldPackage(id))
    ctx.r3.u32 = 1;
}
