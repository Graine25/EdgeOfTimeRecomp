#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <rex/types.h>

#include <plume_render_interface.h>
#include <atomic>
#include <memory>

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

inline i32 PolygonOffsetUnits(float offset) {
  const float layers = std::ceil(std::fabs(offset) * static_cast<float>(1u << 21));
  const i32 units = static_cast<i32>(std::min(layers, 1.0e8f)) << 3;
  return offset < 0.0f ? -units : units;
}

void CanonicalizePipelineState(PipelineState &st, u32 spec_mask, u32 stream_mask);

plume::RenderPipeline *GetOrCreatePipeline(VideoState &s, const PipelineState &state,
                                           bool worker = false,
                                           PsoSource source = PsoSource::Draw,
                                           u16 template_index = 0xFFFF);

void PsoCachePrecache();

void PsoCacheFlushIfDirty(bool force);

void PsoCacheSetLoadingScreen(bool on);
bool PsoCacheInLoadingScreen();

void PsoCacheOnPackageLoad(u32 id);
bool PsoCacheHoldPackage(u32 id);
bool PsoCacheWaitsAllowed();

}

namespace eot::gpu {

struct PsoRecord;

class CompileToken {
public:
  u32 Total() const { return total_.load(std::memory_order_acquire); }
  u32 Pending() const { return pending_.load(std::memory_order_acquire); }
  void AddPending() {
    pending_.fetch_add(1, std::memory_order_acq_rel);
    total_.fetch_add(1, std::memory_order_acq_rel);
  }
  void ReleasePending() { pending_.fetch_sub(1, std::memory_order_acq_rel); }

private:
  std::atomic<u32> pending_{0};
  std::atomic<u32> total_{0};
};
using TokenPtr = std::shared_ptr<CompileToken>;

void PsoPrecacheStart();
void PsoPrecacheStop();

void PsoPrecacheSetLoading(bool loading);

bool PsoPrecacheEnqueue(const PsoRecord &rec, PsoSource source, bool priority,
                        TokenPtr token = nullptr);

void PsoPrecacheBeginLoad();
TokenPtr PsoPrecacheCurrentToken();
bool PsoPrecacheWaitLoad(u32 max_ms);
void PsoPrecacheEndLoad();

bool PsoPrecacheKnown(u64 key, PsoSource *source);

struct PsoPrecacheStats {
  u32 queued = 0, built = 0, existing = 0, skipped = 0, failed = 0;
  u32 priorityPending = 0, backgroundPending = 0, threads = 0;
};
PsoPrecacheStats PsoPrecacheGetStats();

enum class PsoBuildResult { Built, Existing, Skipped, Failed };
PsoBuildResult BuildPipelineFromRecord(VideoState &s, const PsoRecord &rec, PsoSource source);

}
