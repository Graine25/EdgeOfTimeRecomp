#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <rex/types.h>

#include <plume_render_interface.h>
#include <atomic>
#include <memory>
#include <vector>

namespace eot::gpu {

struct VideoState;
struct InputLayout;
struct PsoRecord;

struct PipelineState {
  const plume::RenderShader *vs;
  const plume::RenderShader *ps;
  const InputLayout *layout;
  u32 drawnSpec;
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
  bool velocity;
};
constexpr size_t kPipelineKeyOffset = offsetof(PipelineState, vsHash);

enum class PsoSource : u8 { Draw = 0, Recorded = 1, Local = 2, Asset = 3 };

void ZeroPipelineState(PipelineState &state);
u64 HashPipelineState(const PipelineState &state);

inline i32 PolygonOffsetUnits(float offset) {
  const float layers = std::ceil(std::fabs(offset) * static_cast<float>(1u << 21));
  const i32 units = static_cast<i32>(std::min(layers, 1.0e8f)) << 3;
  return offset < 0.0f ? -units : units;
}

bool DynamicDepthBias();

void CanonicalizePipelineState(PipelineState &st, u32 spec_mask, u32 stream_mask);

plume::RenderPipeline *GetOrCreatePipeline(VideoState &s, const PipelineState &state,
                                           bool worker = false,
                                           PsoSource source = PsoSource::Draw,
                                           bool *deferred = nullptr);

void PsoCachePrecache();

void PsoCacheFlushIfDirty(bool force);
void PipelineCacheCounts(u32 *alive, u32 *used);

void PsoCacheSetLoadingScreen(bool on);
bool PsoCacheInLoadingScreen();

void PsoCacheOnPackageLoad(u32 id, bool level);
void PsoCacheOnPackageUnload(u32 id);
bool PsoCacheHoldPackage(u32 id);

u32 PsoCacheQueue(const PsoRecord &r, PsoSource source, bool background);

}

namespace eot::gpu {

struct PsoRecord;

enum class PsoLane : u8 { Load = 0, Background = 1 };

using PsoPending = std::shared_ptr<std::atomic<u32>>;

void PsoPrecacheStart();
void PsoPrecacheStop();
void PsoPrecacheSetLoading(bool loading);
void PsoPrecacheBoot(const PsoPending &boot_rows);

bool PsoPrecacheEnqueue(const PsoRecord &rec, PsoSource source, PsoLane lane,
                        const PsoPending &own = nullptr);
bool PsoPrecacheBuildNow(const PipelineState &st, u64 key);
bool PsoPrecacheKnown(u64 key);
void PsoPrecacheForget(const std::vector<u64> &keys);
u32 PsoPrecacheScreenPending();

struct PsoPrecacheStats {
  u32 queued = 0, built = 0, existing = 0, skipped = 0, failed = 0;
  u32 urgentBuilt = 0, urgentPending = 0, screenPending = 0, loadPending = 0, backgroundPending = 0;
  u32 threads = 0;
};
PsoPrecacheStats PsoPrecacheGetStats();

enum class PsoBuildResult { Built, Existing, Skipped, Failed };
PsoBuildResult BuildPipelineFromRecord(VideoState &s, const PsoRecord &rec, PsoSource source);

}
