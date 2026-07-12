#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/pipeline/vertex_layout.h"

namespace eot::gpu {

struct PipelineKey {
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  u32 vertexSpecConstants = 0;
  u32 pixelSpecConstants = 0;
  plume::RenderFormat renderTargetFormat = plume::RenderFormat::UNKNOWN;
  plume::RenderFormat depthFormat = plume::RenderFormat::UNKNOWN;
  plume::RenderPrimitiveTopology topology =
      plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  u32 sampleCount = 1;

  u64 inputLayoutHash = 0;

  InputLayout layout;

  u32 blendControl = 0;

  u64 stateHash = 0;

  bool operator==(const PipelineKey &o) const {
    return vertexShaderHash == o.vertexShaderHash &&
           pixelShaderHash == o.pixelShaderHash &&
           vertexSpecConstants == o.vertexSpecConstants &&
           pixelSpecConstants == o.pixelSpecConstants &&
           renderTargetFormat == o.renderTargetFormat &&
           depthFormat == o.depthFormat && topology == o.topology &&
           sampleCount == o.sampleCount &&
           inputLayoutHash == o.inputLayoutHash && stateHash == o.stateHash;
  }
};

struct PipelineKeyHash {
  size_t operator()(const PipelineKey &k) const;
};

plume::RenderPipeline *GetOrCreatePipeline(const PipelineKey &key,
                                           const InputLayout &layout);

bool BuildPipelineKeyForCurrentState(u32 device_va, PipelineKey &out);

void NotePipelineUndescribable();
void LogPipelineStats();

}
