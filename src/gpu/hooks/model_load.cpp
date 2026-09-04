#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/pipeline/pso_predictor.h"
#include "gpu/settings.h"

REX_EXTERN(__imp__sub_82108330);
REX_EXTERN(__imp__sub_8216D0C8);
REX_EXTERN(__imp__sub_8211C5F8);

REX_HOOK_RAW(sub_8211C5F8) {
  __imp__sub_8211C5F8(ctx, base);
  eot::gpu::PredictorNoteShaderBundle(ctx.r3.u32);
}

REX_HOOK_RAW(sub_82108330) {
  const uint32_t material = ctx.r3.u32;
  __imp__sub_82108330(ctx, base);
  if (eot::gpu::Settings::PsoPredict())
    eot::gpu::PredictMaterialLoad(material);
}

REX_HOOK_RAW(sub_8216D0C8) {
  const uint32_t model = ctx.r3.u32;
  if (!eot::gpu::Settings::PsoPredict()) {
    __imp__sub_8216D0C8(ctx, base);
    return;
  }
  eot::gpu::PsoPrecacheBeginLoad();
  const uint32_t queued = eot::gpu::PredictModelLoad(model);
  __imp__sub_8216D0C8(ctx, base);
  if (queued) {
    const uint32_t gate_ms = static_cast<uint32_t>(eot::gpu::Settings::PsoGateMs());
    if (gate_ms && !eot::gpu::PsoPrecacheWaitLoad(gate_ms))
      EOT_DEBUG("[pso] predictor: model {:#x} published with pipelines still building after {} ms",
                model, gate_ms);
  }
  eot::gpu::PsoPrecacheEndLoad();
}
