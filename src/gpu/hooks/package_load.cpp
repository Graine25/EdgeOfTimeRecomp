#include <cstdint>
#include <string>

#include <rex/hook.h>
#include <atomic>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_predictor.h"
#include "gpu/pipeline/pso_records.h"

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
  EOT_DEBUG("[pso] GLAPIPackage::Load({:#x} '{}')", id, PackageName(id));
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

REX_EXTERN(__imp__eot_RendererMaterial_Load);
REX_EXTERN(__imp__eot_ModelResource_LoadGeometry);
REX_EXTERN(__imp__eot_BuildShaderBundleWithDecl);

namespace {
std::atomic<uint32_t> g_late_logs{0};
}

REX_HOOK_RAW(eot_BuildShaderBundleWithDecl) {
  __imp__eot_BuildShaderBundleWithDecl(ctx, base);
  eot::gpu::PredictorNoteShaderBundle(ctx.r3.u32);
}

REX_HOOK_RAW(eot_RendererMaterial_Load) {
  const uint32_t material = ctx.r3.u32;
  __imp__eot_RendererMaterial_Load(ctx, base);
  eot::gpu::PredictMaterialLoad(material);
}

REX_HOOK_RAW(eot_ModelResource_LoadGeometry) {
  const uint32_t model = ctx.r3.u32;
  eot::gpu::PsoPrecacheBeginLoad();
  const uint32_t queued = eot::gpu::PredictModelLoad(model);
  __imp__eot_ModelResource_LoadGeometry(ctx, base);
  if (queued && eot::gpu::PsoCacheWaitsAllowed()) {
    const uint32_t gate_ms = eot::gpu::kPsoGateLoadingScreenMs;
    if (!eot::gpu::PsoPrecacheWaitLoad(gate_ms) &&
        g_late_logs.fetch_add(1, std::memory_order_relaxed) < 32)
      EOT_INFO("[pso] predictor: model {:#x} published with pipelines still building after {} ms",
               model, gate_ms);
  }
  eot::gpu::PsoPrecacheEndLoad();
}
