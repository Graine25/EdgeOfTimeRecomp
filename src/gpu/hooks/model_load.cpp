#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/pipeline/pso_predictor.h"
#include "gpu/settings.h"

REX_EXTERN(__imp__eot_RendererMaterial_Load);
REX_EXTERN(__imp__eot_ModelResource_LoadGeometry);
REX_EXTERN(__imp__eot_BuildShaderBundleWithDecl);

REX_HOOK_RAW(eot_BuildShaderBundleWithDecl) {
  __imp__eot_BuildShaderBundleWithDecl(ctx, base);
  eot::gpu::PredictorNoteShaderBundle(ctx.r3.u32);
}

REX_HOOK_RAW(eot_RendererMaterial_Load) {
  const uint32_t material = ctx.r3.u32;
  __imp__eot_RendererMaterial_Load(ctx, base);
  if (eot::gpu::Settings::PsoPredict())
    eot::gpu::PredictMaterialLoad(material);
}

REX_HOOK_RAW(eot_ModelResource_LoadGeometry) {
  const uint32_t model = ctx.r3.u32;
  if (!eot::gpu::Settings::PsoPredict()) {
    __imp__eot_ModelResource_LoadGeometry(ctx, base);
    return;
  }
  eot::gpu::PsoPrecacheBeginLoad();
  const uint32_t queued = eot::gpu::PredictModelLoad(model);
  __imp__eot_ModelResource_LoadGeometry(ctx, base);
  if (queued) {
    const uint32_t gate_ms = static_cast<uint32_t>(eot::gpu::Settings::PsoGateMs());
    if (gate_ms && !eot::gpu::PsoPrecacheWaitLoad(gate_ms))
      EOT_DEBUG("[pso] predictor: model {:#x} published with pipelines still building after {} ms",
                model, gate_ms);
  }
  eot::gpu::PsoPrecacheEndLoad();
}
