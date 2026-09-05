#include "gpu/pipeline/pipeline_cache.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include "core/logging.h"
#include "core/profiling.h"

#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/pipeline/pso_predictor.h"
#include "gpu/pipeline/pso_records.h"
#include "gpu/settings.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/vertex_layout.h"

namespace eot::gpu {

namespace {

struct Entry {
  std::unique_ptr<plume::RenderPipeline> pipeline;
  PsoSource source = PsoSource::Draw;
  bool used = false;
};

struct Cache {
  std::mutex mutex;
  std::unordered_map<u64, Entry> map;
  u32 failures = 0;
  bool capture = false;
  u32 gapBuilds = 0, raceBuilds = 0;
  u64 lastSummaryFrame = 0;
};

Cache &cache() {
  static Cache c;
  return c;
}

const char *SourceName(PsoSource s) {
  switch (s) {
  case PsoSource::CompiledIn:
    return "compiled-in";
  case PsoSource::LocalCsv:
    return "local csv";
  case PsoSource::Predicted:
    return "predicted";
  default:
    return "draw";
  }
}

void CaptureLocked(VideoState &s, const PipelineState &st) {
  if (!st.layout)
    return;
  const InputLayout &l = *st.layout;
  if (l.declRaw.empty() || l.declRaw.size() > sizeof(PsoRecord::declRaw))
    return;
  PsoRecord r{};
  r.state = st;
  r.state.vs = nullptr;
  r.state.ps = nullptr;
  r.state.layout = nullptr;
  r.declCount = static_cast<u32>(l.declRaw.size() / sizeof(DeclElement));
  std::memcpy(r.declRaw, l.declRaw.data(), l.declRaw.size());
  r.frame = s.guest_frames;
  PsoCaptureAdd(r);
}

}

void ZeroPipelineState(PipelineState &state) { std::memset(&state, 0, sizeof(state)); }

u64 HashPipelineState(const PipelineState &state) {
  return XXH3_64bits(reinterpret_cast<const u8 *>(&state) + kPipelineKeyOffset,
                     sizeof(state) - kPipelineKeyOffset);
}

plume::RenderPipeline *GetOrCreatePipeline(VideoState &s, const PipelineState &st, bool worker,
                                           PsoSource source) {
  auto &c = cache();
  const u64 key = HashPipelineState(st);
  {
    std::lock_guard lock(c.mutex);
    auto it = c.map.find(key);
    if (it != c.map.end()) {
      if (!worker)
        it->second.used = true;
      return it->second.pipeline.get();
    }
  }
  std::unique_ptr<PerfScope> perf_scope;
  if (!worker) {
    perf_scope = std::make_unique<PerfScope>(s.perf.pso_ms);
    s.perf.psos++;
  }

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = st.vs;
  desc.pixelShader = st.ps;

  std::vector<plume::RenderInputSlot> slots;
  if (st.layout) {
    for (u32 slot = 0; slot < 16; ++slot) {
      if (st.layout->streamMask & (1u << slot))
        slots.emplace_back(slot, st.strides[slot]);
    }
    if (st.layout->needsSyntheticSlot)
      slots.emplace_back(kSyntheticVertexSlot, 0u);
    desc.inputSlots = slots.data();
    desc.inputSlotsCount = static_cast<u32>(slots.size());
    desc.inputElements = st.layout->elements.data();
    desc.inputElementsCount = static_cast<u32>(st.layout->elements.size());
  }

  desc.primitiveTopology = st.topology;
  desc.cullMode = st.cull;
  desc.frontFace = st.frontFace;
  desc.depthClipEnabled = st.depthClip;
  desc.depthBias = st.depthBias;
  desc.slopeScaledDepthBias = st.slopeScaledDepthBias;
  desc.depthBiasClamp = 0.0f;
  desc.depthEnabled = st.depthEnable;
  desc.depthWriteEnabled = st.depthWrite;
  desc.depthFunction = st.depthFunc;
  desc.stencilEnabled = st.stencilEnable;
  desc.stencilReadMask = st.stencilReadMask;
  desc.stencilWriteMask = st.stencilWriteMask;
  desc.stencilReference = st.stencilRef;
  desc.stencilFrontFace = st.stencilFront;
  desc.stencilBackFace = st.stencilBack;
  desc.multisampling.sampleCount = st.sampleCount > 1 ? st.sampleCount : 1;
  desc.alphaToCoverageEnabled = st.alphaToCoverage;
  desc.renderTargetCount = st.rtCount;
  for (u32 i = 0; i < st.rtCount && i < 4; ++i) {
    desc.renderTargetFormat[i] = st.rtFormats[i];
    desc.renderTargetBlend[i] = st.blend[i];
  }
  desc.depthTargetFormat = st.dsFormat;

  EOT_CPU_ZONE("pipeline build");
  auto pso = CreateHostGraphicsPipeline(s.device.get(), desc, "guest-draw");
  std::lock_guard lock(c.mutex);
  if (!pso) {
    if (c.failures++ < 32) {
      EOT_ERROR("[pso] creation failed ({}): vs={:016x} ps={:016x} elements={} rt={} ds={} topo={}",
                SourceName(source), st.vsHash, st.psHash,
                st.layout ? st.layout->elements.size() : 0, st.rtCount,
                static_cast<u32>(st.dsFormat), static_cast<u32>(st.topology));
    }
    c.map.emplace(key, Entry{nullptr, source, false});
    return nullptr;
  }
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.pipeline.get();
  auto *raw = pso.get();
  if (!worker) {
    PsoSource known;
    const bool race = PsoPrecacheKnown(key, &known);
    (race ? c.raceBuilds : c.gapBuilds)++;
    static u32 created = 0;
    if (created++ < 400 || !race) {
      const u32 vs_va = s.current_vs_va;
      EOT_INFO("[pso] #{} render-thread build ({}) key={:016x} vs={:016x} ps={:016x} spec={:#x} "
               "depth={}{} func{} stencil={} cull={} rt0fmt={} ds={} topo={} vsVa={:#x} psVa={:#x} nodes={}/{} origin={} vsCanon={:016x} psCanon={:016x}",
               created, race ? std::string("race with ") + SourceName(known) : "GAP, captured",
               key, st.vsHash, st.psHash, st.spec, st.depthEnable ? "on" : "off",
               st.depthWrite ? "w" : "", static_cast<u32>(st.depthFunc), st.stencilEnable,
               static_cast<u32>(st.cull), static_cast<u32>(st.rtFormats[0]),
               static_cast<u32>(st.dsFormat), static_cast<u32>(st.topology), vs_va, s.current_ps_va, PsoPredictorDescribeObject(vs_va),
               s.current_ps_va ? PsoPredictorDescribeObject(s.current_ps_va) : std::string("-"),
               s.current_origin, CanonicalShaderHash(st.vsHash), CanonicalShaderHash(st.psHash));
    }
    if (!race && c.capture)
      CaptureLocked(s, st);
  }
  c.map.emplace(key, Entry{std::move(pso), source, !worker});
  return raw;
}

PsoBuildResult BuildPipelineFromRecord(VideoState &s, const PsoRecord &r, PsoSource source) {
  if (!s.ready || !s.device)
    return PsoBuildResult::Skipped;
  {
    auto &c = cache();
    std::lock_guard lock(c.mutex);
    if (c.map.count(HashPipelineState(r.state)))
      return PsoBuildResult::Existing;
  }
  const ShaderCacheEntry *vs_entry = FindShaderCacheEntry(r.state.vsHash);
  if (!vs_entry || r.declCount == 0 || r.declCount > 32)
    return PsoBuildResult::Skipped;
  PipelineState st = r.state;
  st.vs = GetHostShaderByHash(s, r.state.vsHash, r.state.spec, false, true);
  st.ps = r.state.psHash
              ? GetHostShaderByHash(s, r.state.psHash, r.state.spec, true, true)
              : nullptr;
  if (!st.vs || (r.state.psHash && !st.ps))
    return PsoBuildResult::Skipped;
  std::vector<VertexInput> inputs;
  VertexInputsFromEntry(*vs_entry, inputs);
  st.layout = GetInputLayoutFromRaw(r.state.vsHash, inputs, r.declRaw, r.declCount);
  if (!st.layout)
    return PsoBuildResult::Skipped;
  st.layoutKey = st.layout->key;
  return GetOrCreatePipeline(s, st, true, source) ? PsoBuildResult::Built
                                                            : PsoBuildResult::Failed;
}

void PsoCachePrecache() {
  auto &c = cache();
  auto &s = state();
  if (!s.ready || !s.device)
    return;
  const std::string dir = Settings::PsoDir();
  c.capture = Settings::PsoCapture() && !dir.empty();
  if (c.capture)
    PsoCaptureConfigure(dir, Settings::PsoTag());
  PsoPrecacheStart();

  size_t compiled_in = 0, local = 0, queued = 0;
  if (Settings::PsoCompiledIn()) {
    for (const PsoRecord &r : CompiledInPipelines()) {
      ++compiled_in;
      queued += PsoPrecacheEnqueue(r, PsoSource::CompiledIn, false) ? 1 : 0;
    }
  }
  std::vector<PsoRecord> rows;
  local = LoadPsoCsvDir(dir, rows);
  for (const PsoRecord &r : rows)
    queued += PsoPrecacheEnqueue(r, PsoSource::LocalCsv, false) ? 1 : 0;
  EOT_INFO("[pso] boot: {} compiled-in{} + {} local rows -> {} queued on the background lane "
           "({} duplicate); templates: {}; {}",
           compiled_in, Settings::PsoCompiledIn() ? "" : " (disabled)", local, queued,
           compiled_in + local - queued, CompiledInTemplates().size(),
           c.capture ? "capturing gaps to " + dir : "capture off");
}

void PsoCacheFlushIfDirty(bool force) {
  auto &s = state();
  PsoCaptureFlush(force, s.guest_frames);
  auto &c = cache();
  if (!force && s.guest_frames < c.lastSummaryFrame + 600)
    return;
  u32 by_source[4] = {}, used_by_source[4] = {}, total = 0;
  u32 gaps, races;
  {
    std::lock_guard lock(c.mutex);
    c.lastSummaryFrame = s.guest_frames;
    for (const auto &[key, e] : c.map) {
      if (!e.pipeline)
        continue;
      const u32 i = static_cast<u32>(e.source) & 3;
      by_source[i]++;
      used_by_source[i] += e.used ? 1 : 0;
      total++;
    }
    gaps = c.gapBuilds;
    races = c.raceBuilds;
    c.gapBuilds = c.raceBuilds = 0;
  }
  const PsoPrecacheStats ps = PsoPrecacheGetStats();
  const PsoPredictorStats pr = PsoPredictorGetStats();
  if (total == 0 && ps.queued == 0)
    return;
  EOT_INFO("[pso] {} pipelines: draw {} | compiled-in {} ({} used) | local {} ({} used) | "
           "predicted {} ({} used) | render-thread builds since last: {} gaps, {} races | pool: "
           "{} queued, {} built, {} existing, {} skipped, {} failed, pending prio {} bg {} | predictor: "
           "{} models, {} materials, {} slots, {} queued, {} without template",
           total, by_source[0], by_source[1], used_by_source[1], by_source[2],
           used_by_source[2], by_source[3], used_by_source[3], gaps, races, ps.queued, ps.built,
           ps.existing, ps.skipped, ps.failed, ps.priorityPending, ps.backgroundPending, pr.models,
           pr.materials, pr.slots, pr.queued, pr.noTemplate);
}

}
