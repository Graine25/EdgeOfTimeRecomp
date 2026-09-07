#include <atomic>
#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/pipeline/pso_predictor.h"
#include "gpu/pipeline/pso_records.h"

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
  if (queued) {
    const uint32_t gate_ms = eot::gpu::PsoCacheInLoadingScreen()
                                 ? eot::gpu::kPsoGateLoadingScreenMs
                                 : eot::gpu::kPsoGateMs;
    if (!eot::gpu::PsoPrecacheWaitLoad(gate_ms) &&
        g_late_logs.fetch_add(1, std::memory_order_relaxed) < 32)
      EOT_INFO("[pso] predictor: model {:#x} published with pipelines still building after {} ms",
               model, gate_ms);
  }
  eot::gpu::PsoPrecacheEndLoad();
}
