#include <cstdint>

#include <rex/hook.h>

#include "gpu/pipeline/pso_predictor.h"

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
  eot::gpu::PredictMaterialLoad(material);
}

REX_HOOK_RAW(eot_ModelResource_LoadGeometry) {
  const uint32_t model = ctx.r3.u32;
  eot::gpu::PredictModelLoad(model);
  __imp__eot_ModelResource_LoadGeometry(ctx, base);
}
