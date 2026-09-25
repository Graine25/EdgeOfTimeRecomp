#pragma once

#include <rex/types.h>

namespace eot::gpu {

void PsoAssetsMaterialLoaded(u32 material_va);
void PsoAssetsModelLoading(u32 model_va);
void PsoAssetsShaderBundle(u32 bundle_va);

struct PsoAssetStats {
  u32 materials = 0, models = 0, bundles = 0;
  u32 slots = 0, recorded = 0, layouts = 0, alpha = 0, unknown = 0, queued = 0;
};
PsoAssetStats PsoAssetsGetStats();

}
