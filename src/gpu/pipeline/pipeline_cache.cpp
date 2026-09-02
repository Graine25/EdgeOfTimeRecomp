#include "gpu/pipeline/pipeline_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xxhash.h>

#include "core/logging.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/pipeline/pso_records.h"
#include "gpu/settings.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/vertex_layout.h"

namespace eot::gpu {

namespace {

struct Cache {
  std::mutex mutex;
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> map;
  u32 failures = 0;
  std::unordered_set<u64> known;
  bool capture = false;
  u32 precached = 0;
};

Cache &cache() {
  static Cache c;
  return c;
}

void CaptureLocked(Cache &c, VideoState &s, u64 key, const PipelineState &st) {
  if (!c.capture || !st.layout || c.known.count(key))
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
  c.known.insert(key);
  PsoCaptureAdd(r);
}

}

void ZeroPipelineState(PipelineState &state) { std::memset(&state, 0, sizeof(state)); }

u64 HashPipelineState(const PipelineState &state) {
  return XXH3_64bits(reinterpret_cast<const u8 *>(&state) + kPipelineKeyOffset,
                     sizeof(state) - kPipelineKeyOffset);
}

plume::RenderPipeline *GetOrCreatePipeline(VideoState &s, const PipelineState &st, bool worker) {
  auto &c = cache();
  const u64 key = HashPipelineState(st);
  {
    std::lock_guard lock(c.mutex);
    auto it = c.map.find(key);
    if (it != c.map.end())
      return it->second.get();
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

  auto pso = CreateHostGraphicsPipeline(s.device.get(), desc, "guest-draw");
  std::lock_guard lock(c.mutex);
  if (!pso) {
    if (c.failures++ < 32) {
      EOT_ERROR("[pso] creation failed: vs={:016x} ps={:016x} elements={} rt={} ds={} topo={}",
                st.vsHash, st.psHash, st.layout ? st.layout->elements.size() : 0, st.rtCount,
                static_cast<u32>(st.dsFormat), static_cast<u32>(st.topology));
    }
    c.map.emplace(key, nullptr);
    return nullptr;
  }
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.get();
  auto *raw = pso.get();
  if (worker) {
    c.precached++;
  } else {
    const bool gap = !c.known.count(key);
    static u32 created = 0;
    if (created++ < 400 || gap) {
      EOT_INFO("[pso] #{} render-thread build ({}) key={:016x} vs={:016x} ps={:016x} spec={:#x} "
               "depth={}{} func{} stencil={} cull={} rt0fmt={} ds={} topo={}",
               created, gap ? "gap, captured" : "known, precache race", key, st.vsHash,
               st.psHash, st.spec, st.depthEnable ? "on" : "off", st.depthWrite ? "w" : "",
               static_cast<u32>(st.depthFunc), st.stencilEnable, static_cast<u32>(st.cull),
               static_cast<u32>(st.rtFormats[0]), static_cast<u32>(st.dsFormat),
               static_cast<u32>(st.topology));
    }
    CaptureLocked(c, s, key, st);
  }
  c.map.emplace(key, std::move(pso));
  return raw;
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

  std::vector<PsoRecord> records = CompiledInPipelines();
  const size_t compiled_in = records.size();
  const size_t local = LoadPsoCsvDir(dir, records);
  {
    std::lock_guard lock(c.mutex);
    for (const PsoRecord &r : records)
      c.known.insert(HashPipelineState(r.state));
  }
  if (records.empty()) {
    EOT_INFO("[pso] nothing to precache (no compiled-in rows, no *.csv in {}); {}", dir,
             c.capture ? "capturing this session" : "capture off");
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::atomic<size_t> next{0};
  std::atomic<u32> skipped{0}, dupes{0};
  std::mutex seen_mutex;
  std::unordered_set<u64> seen;
  auto work = [&]() {
    for (;;) {
      const size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= records.size())
        return;
      const PsoRecord &r = records[i];
      {
        std::lock_guard lock(seen_mutex);
        if (!seen.insert(HashPipelineState(r.state)).second) {
          dupes++;
          continue;
        }
      }
      const ShaderCacheEntry *vs_entry = FindShaderCacheEntry(r.state.vsHash);
      if (!vs_entry || r.declCount == 0 || r.declCount > 32) {
        skipped++;
        continue;
      }
      PipelineState st = r.state;
      st.vs = GetHostShaderByHash(s, r.state.vsHash, r.state.spec, false, true);
      st.ps = r.state.psHash
                  ? GetHostShaderByHash(s, r.state.psHash, r.state.spec, true, true)
                  : nullptr;
      if (!st.vs || (r.state.psHash && !st.ps)) {
        skipped++;
        continue;
      }
      std::vector<VertexInput> inputs;
      VertexInputsFromEntry(*vs_entry, inputs);
      st.layout = GetInputLayoutFromRaw(r.state.vsHash, inputs, r.declRaw, r.declCount);
      if (!st.layout || st.layout->key != r.state.layoutKey) {
        skipped++;
        continue;
      }
      GetOrCreatePipeline(s, st, true);
    }
  };
  const u32 hw = std::max(1u, std::thread::hardware_concurrency());
  const u32 threads = std::clamp(hw > 2 ? hw - 2 : 1u, 1u, 8u);
  std::vector<std::thread> pool;
  for (u32 i = 0; i < threads; ++i)
    pool.emplace_back(work);
  for (auto &t : pool)
    t.join();
  const f64 ms =
      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  EOT_INFO("[pso] precached {} pipelines in {:.0f} ms on {} threads: {} compiled-in + {} local "
           "rows, {} duplicate, {} skipped (shader not in cache or layout mismatch); {}",
           c.precached, ms, threads, compiled_in, local, dupes.load(), skipped.load(),
           c.capture ? "capturing gaps to " + dir : "capture off");
}

void PsoCacheFlushIfDirty(bool force) { PsoCaptureFlush(force, state().guest_frames); }

}
