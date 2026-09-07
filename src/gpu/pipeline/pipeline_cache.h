#pragma once

#include <cstddef>

#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

struct VideoState;
struct InputLayout;

struct PipelineState {
  const plume::RenderShader *vs;
  const plume::RenderShader *ps;
  const InputLayout *layout;
  u64 vsHash;
  u64 psHash;
  u64 layoutKey;
  u32 spec;
  u32 strides[16];
  plume::RenderPrimitiveTopology topology;
  plume::RenderFormat rtFormats[4];
  u32 rtCount;
  plume::RenderFormat dsFormat;
  u32 sampleCount;
  plume::RenderCullMode cull;
  plume::RenderFrontFace frontFace;
  i32 depthBias;
  float slopeScaledDepthBias;
  float targetScale;
  bool depthClip;
  bool depthEnable;
  bool depthWrite;
  plume::RenderComparisonFunction depthFunc;
  bool stencilEnable;
  u8 stencilReadMask;
  u8 stencilWriteMask;
  u8 stencilRef;
  plume::RenderStencilFaceDesc stencilFront;
  plume::RenderStencilFaceDesc stencilBack;
  plume::RenderBlendDesc blend[4];
  bool alphaToCoverage;
};
constexpr size_t kPipelineKeyOffset = offsetof(PipelineState, vsHash);

enum class PsoSource : u8 { Draw = 0, CompiledIn = 1, LocalCsv = 2, Predicted = 3 };

void ZeroPipelineState(PipelineState &state);
u64 HashPipelineState(const PipelineState &state);

plume::RenderPipeline *GetOrCreatePipeline(VideoState &s, const PipelineState &state,
                                           bool worker = false,
                                           PsoSource source = PsoSource::Draw);

void PsoCachePrecache();

void PsoCacheFlushIfDirty(bool force);

}
