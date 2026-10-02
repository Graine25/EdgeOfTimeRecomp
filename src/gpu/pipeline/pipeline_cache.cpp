#include "gpu/pipeline/pipeline_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <xxhash.h>

#include "core/logging.h"
#include "core/profiling.h"

#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/pipeline/pso_records.h"
#include "gpu/pipeline/pso_set.h"
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
  std::shared_mutex mutex;
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

struct PackageHold {
  TokenPtr token;
  std::chrono::steady_clock::time_point start;
  bool held = false;
};

struct Loading {
  std::mutex mutex;
  std::unordered_map<u16, std::vector<PsoRecord>> sets;
  std::unordered_map<u32, PackageHold> holds;
  std::atomic<bool> screen{false};
  std::atomic<u32> currentPackage{0};
  std::atomic<u32> levelPackage{0};
  std::unordered_set<u16> resident;
  std::vector<u32> casterRows;
  std::vector<PsoShadowValue> shadowValues;
  u64 screenSinceFrame = 0;
  u32 screens = 0, holdsCount = 0, setsQueued = 0, drawnCaptured = 0, prefetched = 0;
  u32 queuedCaptured = 0, queuedDerived = 0, queuedVariants = 0;
  f64 holdMs = 0;
};

Loading &loading() {
  static Loading l;
  return l;
}

struct SetJobs {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  bool started = false;
};

void PostSetJob(std::function<void()> job) {
  static SetJobs *const q = new SetJobs;
  {
    std::lock_guard lock(q->mutex);
    q->jobs.push_back(std::move(job));
    if (!q->started) {
      q->started = true;
      std::thread([] {
        for (;;) {
          std::function<void()> next;
          {
            std::unique_lock lock(q->mutex);
            q->cv.wait(lock, [] { return !q->jobs.empty(); });
            next = std::move(q->jobs.front());
            q->jobs.pop_front();
          }
          next();
        }
      }).detach();
    }
  }
  q->cv.notify_one();
}

const char *SourceName(PsoSource s) {
  switch (s) {
  case PsoSource::CompiledIn:
    return "captured";
  case PsoSource::LocalCsv:
    return "local csv";
  case PsoSource::Derived:
    return "derived";
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
  r.state.spec = st.drawnSpec;
  if (state().host_msaa_samples > 1)
    r.msaa = st.sampleCount > 1 ? kPsoMsaaMulti : kPsoMsaaSingle;
  r.state.sampleCount = 1;
  r.declCount = static_cast<u32>(l.declRaw.size() / sizeof(DeclElement));
  std::memcpy(r.declRaw, l.declRaw.data(), l.declRaw.size());
  r.frame = s.guest_frames;
  auto &ld = loading();
  const u32 level = ld.levelPackage.load(std::memory_order_relaxed);
  const u32 last = ld.currentPackage.load(std::memory_order_relaxed);
  if (level)
    r.packages[r.packageCount++] = static_cast<u16>(level);
  if (last && last != level)
    r.packages[r.packageCount++] = static_cast<u16>(last);
  PsoCaptureAdd(r);
}

u32 QueueRecord(const PsoRecord &r, PsoSource source, PsoLane lane, const TokenPtr &token,
                u8 msaa) {
  u32 n = 0;
  const u32 samples = state().host_msaa_samples;
  const bool multi = samples > 1 && r.state.sampleCount == 1;
  if (multi && (msaa == 0 || (msaa & kPsoMsaaMulti))) {
    PsoRecord t = r;
    t.state.sampleCount = samples;
    n += PsoPrecacheEnqueue(t, source, lane, token) ? 1 : 0;
  }
  if (!multi || msaa == 0 || (msaa & kPsoMsaaSingle))
    n += PsoPrecacheEnqueue(r, source, lane, token) ? 1 : 0;
  return n;
}

struct LiveKeys {
  std::mutex mutex;
  std::vector<u64> hash;
};

LiveKeys &liveKeys() {
  static LiveKeys k;
  return k;
}

u64 LiveVariant(i32 key, u64 own) {
  if (key < 0)
    return own;
  auto &live = liveKeys();
  std::lock_guard lock(live.mutex);
  const u64 h = static_cast<size_t>(key) < live.hash.size() ? live.hash[key] : 0;
  return h ? h : own;
}

bool SetRecord(const PsoSet &set, const PsoSetRow &row, const PsoShadowValue *shadow, PsoRecord *out,
               i32 core = -1) {
  const PsoHashPair hashes{LiveVariant(row.vk, row.vsHash), LiveVariant(row.pk, row.psHash)};
  return PsoSetRecord(set, row, shadow, out, core, &hashes);
}

u32 QueueSetRow(const PsoSet &set, u32 index, PsoLane lane, const TokenPtr &token,
                const std::vector<PsoShadowValue> &shadow) {
  const PsoSetRow &row = set.rows[index];
  const PsoSource source = row.derived ? PsoSource::Derived : PsoSource::CompiledIn;
  PsoRecord r;
  if (row.bias == PsoBias::Shadow) {
    u32 n = 0;
    for (const PsoShadowValue &v : shadow)
      if (SetRecord(set, row, &v, &r))
        n += QueueRecord(r, source, lane, token, kPsoMsaaSingle);
    return n;
  }
  u32 n = SetRecord(set, row, nullptr, &r) ? QueueRecord(r, source, lane, token, row.msaa) : 0;
  const i32 velocity = set.velocityOf[row.core];
  if (velocity >= 0 && Settings::MotionVectors() && SetRecord(set, row, nullptr, &r, velocity))
    n += QueueRecord(r, source, lane, token, row.msaa);
  return n;
}

void RouteLocal(const PsoRecord &r, size_t *queued, size_t *per_package) {
  if (r.packageCount == 0) {
    *queued += QueueRecord(r, PsoSource::LocalCsv, PsoLane::Recorded, nullptr, r.msaa);
    return;
  }
  auto &l = loading();
  std::lock_guard lock(l.mutex);
  for (u32 i = 0; i < r.packageCount; ++i)
    l.sets[r.packages[i]].push_back(r);
  ++*per_package;
}

}

void ZeroPipelineState(PipelineState &state) { std::memset(&state, 0, sizeof(state)); }

u64 HashPipelineState(const PipelineState &state) {
  return XXH3_64bits(reinterpret_cast<const u8 *>(&state) + kPipelineKeyOffset,
                     sizeof(state) - kPipelineKeyOffset);
}

void CanonicalizePipelineState(PipelineState &st, u32 spec_mask, u32 stream_mask) {
  st.drawnSpec = st.spec & spec_mask;
#if defined(EOT_D3D12)
  st.spec = 0;
#else
  st.spec = st.drawnSpec;
#endif
  if (st.sampleCount == 0)
    st.sampleCount = 1;
  if (!st.depthEnable) {
    st.depthWrite = false;
    st.depthFunc = plume::RenderComparisonFunction::ALWAYS;
    st.depthBias = 0;
    st.slopeScaledDepthBias = 0.0f;
  }
  if (st.depthBias == 0 && st.slopeScaledDepthBias == 0.0f) {
    st.slopeScaledDepthBias = 0.0f;
    st.targetScale = 1.0f;
  }
  if (!st.stencilEnable) {
    st.stencilReadMask = st.stencilWriteMask = st.stencilRef = 0;
    std::memset(&st.stencilFront, 0, sizeof(st.stencilFront));
    std::memset(&st.stencilBack, 0, sizeof(st.stencilBack));
    st.stencilFront.compareFunction = plume::RenderComparisonFunction::ALWAYS;
    st.stencilBack.compareFunction = plume::RenderComparisonFunction::ALWAYS;
    for (plume::RenderStencilFaceDesc *face : {&st.stencilFront, &st.stencilBack}) {
      face->failOp = plume::RenderStencilOp::KEEP;
      face->depthFailOp = plume::RenderStencilOp::KEEP;
      face->passOp = plume::RenderStencilOp::KEEP;
    }
  }
  for (u32 i = 0; i < 4; ++i) {
    plume::RenderBlendDesc &b = st.blend[i];
    if (i >= st.rtCount) {
      std::memset(&b, 0, sizeof(b));
      st.rtFormats[i] = plume::RenderFormat::UNKNOWN;
      continue;
    }
    if (b.renderTargetWriteMask == 0)
      b.blendEnabled = false;
    if (!b.blendEnabled) {
      b.srcBlend = plume::RenderBlend::ONE;
      b.dstBlend = plume::RenderBlend::ZERO;
      b.blendOp = plume::RenderBlendOperation::ADD;
      b.srcBlendAlpha = plume::RenderBlend::ONE;
      b.dstBlendAlpha = plume::RenderBlend::ZERO;
      b.blendOpAlpha = plume::RenderBlendOperation::ADD;
    }
  }
  if (st.rtCount == 0)
    st.alphaToCoverage = false;
  if (stream_mask != 0xFFFFu) {
    for (u32 S = 0; S < 16; ++S)
      if (!(stream_mask & (1u << S)))
        st.strides[S] = 0;
  }
}

plume::RenderPipeline *GetOrCreatePipeline(VideoState &s, const PipelineState &st, bool worker,
                                           PsoSource source) {
  auto &c = cache();
  const u64 key = HashPipelineState(st);
  struct HotPipeline {
    u64 key = 0;
    plume::RenderPipeline *pipeline = nullptr;
  };
  static thread_local HotPipeline hot[256];
  HotPipeline *hot_entry = nullptr;
  if (!worker) {
    hot_entry = &hot[key & 255];
    if (hot_entry->pipeline && hot_entry->key == key) {
      s.perf.pipeline_hot_hits++;
      return hot_entry->pipeline;
    }
  }
  {
    std::shared_lock lock(c.mutex);
    auto it = c.map.find(key);
    if (it != c.map.end()) {
      plume::RenderPipeline *pipeline = it->second.pipeline.get();
      if (!worker && !it->second.used) {
        it->second.used = true;
        if (pipeline && c.capture) {
          CaptureLocked(s, st);
          loading().drawnCaptured++;
        }
      }
      if (hot_entry && pipeline)
        *hot_entry = {key, pipeline};
      return pipeline;
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
  desc.slopeScaledDepthBias = st.slopeScaledDepthBias * st.targetScale;
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

#if !defined(EOT_D3D12)
  const plume::RenderSpecConstant spec_constant(0, st.spec);
  if (st.spec != 0) {
    desc.specConstants = &spec_constant;
    desc.specConstantsCount = 1;
  }
#endif

  EOT_CPU_ZONE("pipeline build");
  auto pso = CreateHostGraphicsPipeline(s.device.get(), desc, "guest-draw");
  std::unique_lock lock(c.mutex);
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
  if (it != c.map.end()) {
    plume::RenderPipeline *pipeline = it->second.pipeline.get();
    if (hot_entry && pipeline)
      *hot_entry = {key, pipeline};
    return pipeline;
  }
  auto *raw = pso.get();
  if (!worker) {
    PsoSource known;
    const bool race = PsoPrecacheKnown(key, &known);
    (race ? c.raceBuilds : c.gapBuilds)++;
    static u32 created = 0;
    if (created++ < 400 || !race) {
      const u32 vs_va = s.current_vs_va;
      EOT_DEBUG("[pso] #{} render-thread build ({}) key={:016x} vs={:016x} ps={:016x} spec={:#x} "
               "depth={}{} func{} stencil={} cull={} rt0fmt={} ds={} topo={} vsVa={:#x} psVa={:#x} "
               "origin={} package={:#x}{}",
               created, race ? std::string("race with ") + SourceName(known) : "GAP, captured",
               key, st.vsHash, st.psHash, st.spec, st.depthEnable ? "on" : "off",
               st.depthWrite ? "w" : "", static_cast<u32>(st.depthFunc), st.stencilEnable,
               static_cast<u32>(st.cull), static_cast<u32>(st.rtFormats[0]),
               static_cast<u32>(st.dsFormat), static_cast<u32>(st.topology), vs_va,
               s.current_ps_va, s.current_origin,
               loading().currentPackage.load(std::memory_order_relaxed),
               loading().screen.load(std::memory_order_relaxed) ? " (loading screen)" : "");
    }
    if (c.capture) {
      CaptureLocked(s, st);
      loading().drawnCaptured++;
    }
  }
  c.map.emplace(key, Entry{std::move(pso), source, !worker});
  if (hot_entry)
    *hot_entry = {key, raw};
  return raw;
}

PsoBuildResult BuildPipelineFromRecord(VideoState &s, const PsoRecord &r, PsoSource source) {
  if (!s.ready || !s.device)
    return PsoBuildResult::Skipped;
  {
    auto &c = cache();
    std::shared_lock lock(c.mutex);
    if (c.map.count(HashPipelineState(r.state)))
      return PsoBuildResult::Existing;
  }
  const ShaderCacheEntry *vs_entry = FindShaderCacheEntry(r.state.vsHash);
  if (!vs_entry || r.declCount == 0 || r.declCount > 32)
    return PsoBuildResult::Skipped;
  PipelineState st = r.state;
  const ShaderCacheEntry *ps_entry = r.state.psHash ? FindShaderCacheEntry(r.state.psHash) : nullptr;
  const VsVariant variant = VsVariantFor(vs_entry, ps_entry, r.state.psHash == 0, r.state.velocity);
  if (r.state.velocity && (variant != VsVariant::Velocity || !Settings::MotionVectors()))
    return PsoBuildResult::Skipped;
  st.vs = GetHostShaderByHash(s, r.state.vsHash, r.state.spec, false, true, variant);
  st.ps = r.state.psHash
              ? GetHostShaderByHash(s, r.state.psHash, r.state.spec, true, true,
                                    r.state.velocity ? VsVariant::Velocity : VsVariant::Trimmed)
              : nullptr;
  if (!st.vs || (r.state.psHash && !st.ps))
    return PsoBuildResult::Skipped;
  std::vector<VertexInput> inputs;
  VertexInputsFromEntry(*vs_entry, inputs);
  st.layout = GetInputLayoutFromRaw(r.state.vsHash, inputs, r.declRaw, r.declCount);
  if (!st.layout)
    return PsoBuildResult::Skipped;
  st.layoutKey = st.layout->key;
  st.spec = (st.spec & ~kSpecLayoutBits) | st.layout->spec;
  u32 spec_mask = vs_entry->specConstantsMask;
  if (r.state.psHash) {
    if (const ShaderCacheEntry *e = FindShaderCacheEntry(r.state.psHash))
      spec_mask |= e->specConstantsMask;
  }
  CanonicalizePipelineState(st, spec_mask, st.layout->streamMask);
  return GetOrCreatePipeline(s, st, true, source)
             ? PsoBuildResult::Built
             : PsoBuildResult::Failed;
}

void PsoCachePrecache() {
  auto &c = cache();
  auto &s = state();
  if (!s.ready || !s.device)
    return;
  PsoCaptureConfigure();
  {
    std::unique_lock lock(c.mutex);
    c.capture = true;
  }
  PsoPrecacheStart();

  const PsoSet &set = CompiledInSet();
  size_t queued = 0, per_package = 0, boot_casters = 0;
  for (const u32 index : set.boot) {
    if (set.rows[index].bias == PsoBias::Shadow) {
      auto &l = loading();
      std::lock_guard lock(l.mutex);
      l.casterRows.push_back(index);
      ++boot_casters;
      continue;
    }
    queued += QueueSetRow(set, index, PsoLane::Recorded, nullptr, {});
  }
  std::vector<PsoRecord> rows;
  const size_t local = LoadPsoCsvDir(kPsoDir, rows);
  for (const PsoRecord &r : rows)
    RouteLocal(r, &queued, &per_package);
  EOT_INFO("[pso] boot: set of {} rows ({} captured, {} derived) over {} packages; {} boot rows "
           "({} casters wait for a shadow camera) and {} local rows ({} for their packages) -> {} "
           "queued; capturing every pipeline drawn to {}/ as '{}'",
           set.rows.size(), set.captured, set.derived, set.packages.size(), set.boot.size(),
           boot_casters, local, per_package, queued, kPsoDir, PsoSessionTag());
}

void PsoCacheSetLoadingScreen(bool on) {
  auto &l = loading();
  auto &s = state();
  const bool was = l.screen.exchange(on, std::memory_order_acq_rel);
  if (was == on)
    return;
  PsoPrecacheSetLoading(on);
  const PsoPrecacheStats ps = PsoPrecacheGetStats();
  if (on) {
    l.screenSinceFrame = s.guest_frames;
    std::lock_guard lock(l.mutex);
    l.screens++;
    EOT_INFO("[pso] loading screen up at frame {} (pool pending captured {} derived {} prefetch {})",
             s.guest_frames, ps.recordedPending, ps.derivedPending, ps.backgroundPending);
    return;
  }
  u32 pending = 0;
  {
    std::lock_guard lock(l.mutex);
    for (auto &[id, h] : l.holds)
      pending += h.token ? h.token->Pending() : 0;
    l.holds.clear();
  }
  EOT_INFO("[pso] loading screen down at frame {} after {} frames (pool pending captured {} "
           "derived {} prefetch {}, {} package pipelines still building)",
           s.guest_frames, s.guest_frames - l.screenSinceFrame, ps.recordedPending,
           ps.derivedPending, ps.backgroundPending, pending);
}

bool PsoCacheInLoadingScreen() { return loading().screen.load(std::memory_order_acquire); }

bool PsoCacheWaitsAllowed() {
  return loading().screen.load(std::memory_order_acquire);
}

void PsoCacheOnPackageLoad(u32 id, bool level) {
  auto &l = loading();
  if (id == 0 || id >= 0x1000)
    return;
  l.currentPackage.store(id, std::memory_order_relaxed);
  if (level)
    l.levelPackage.store(id, std::memory_order_relaxed);
  const PsoSet &set = CompiledInSet();
  const auto package = [&](u16 p) -> const PsoSetPackage * {
    auto it = set.packages.find(p);
    return it == set.packages.end() ? nullptr : &it->second;
  };
  std::vector<u16> fresh, prefetch;
  std::vector<u32> casters;
  std::vector<PsoShadowValue> shadow;
  std::vector<PsoRecord> local;
  bool new_offsets = false;
  {
    std::lock_guard lock(l.mutex);
    std::vector<u16> stack{static_cast<u16>(id)};
    while (!stack.empty()) {
      const u16 p = stack.back();
      stack.pop_back();
      if (!l.resident.insert(p).second)
        continue;
      fresh.push_back(p);
      if (const PsoSetPackage *pk = package(p))
        stack.insert(stack.end(), pk->parents.begin(), pk->parents.end());
    }
    const PsoSetPackage *own = package(static_cast<u16>(id));
    if (level) {
      l.shadowValues = own ? own->shadow : std::vector<PsoShadowValue>{};
      new_offsets = !l.shadowValues.empty();
    }
    for (const u16 p : fresh)
      if (const PsoSetPackage *pk = package(p))
        for (const u32 index : pk->rows)
          if (set.rows[index].bias == PsoBias::Shadow)
            l.casterRows.push_back(index);
    shadow = l.shadowValues;
    if (new_offsets)
      casters = l.casterRows;
    if (level && own) {
      std::vector<u16> frontier{static_cast<u16>(id)};
      for (u32 depth = 0; depth < 2; ++depth) {
        std::vector<u16> next;
        for (const u16 p : frontier)
          if (const PsoSetPackage *pk = package(p))
            for (const u16 c : pk->children) {
              const PsoSetPackage *ck = package(c);
              if (!ck || !ck->level || l.resident.count(c))
                continue;
              prefetch.push_back(c);
              next.push_back(c);
            }
        frontier = std::move(next);
      }
    }
    if (auto it = l.sets.find(static_cast<u16>(id)); it != l.sets.end())
      local = it->second;
  }
  if (fresh.empty() || fresh.front() != id)
    fresh.insert(fresh.begin(), static_cast<u16>(id));
  std::sort(prefetch.begin(), prefetch.end());
  TokenPtr token = std::make_shared<CompileToken>();
  token->AddPending();
  {
    std::lock_guard lock(l.mutex);
    l.holds[id] = PackageHold{token, std::chrono::steady_clock::now(), false};
  }
  const bool screen = l.screen.load(std::memory_order_relaxed);
  PostSetJob([id, level, new_offsets, screen, token, fresh = std::move(fresh),
              prefetch = std::move(prefetch), casters = std::move(casters),
              shadow = std::move(shadow), local = std::move(local)] {
    auto &l = loading();
    const PsoSet &set = CompiledInSet();
    const auto package = [&set](u16 p) -> const PsoSetPackage * {
      auto it = set.packages.find(p);
      return it == set.packages.end() ? nullptr : &it->second;
    };
    u32 captured = 0, derived = 0, crossed = 0, ahead = 0;
    for (const u16 p : fresh)
      if (const PsoSetPackage *pk = package(p))
        for (const u32 index : pk->rows)
          if (!set.rows[index].derived && set.rows[index].bias != PsoBias::Shadow)
            captured += QueueSetRow(set, index, PsoLane::Recorded, token, shadow);
    for (const PsoRecord &r : local)
      captured += QueueRecord(r, PsoSource::LocalCsv, PsoLane::Recorded, token, r.msaa);
    for (const u16 p : fresh)
      if (const PsoSetPackage *pk = package(p)) {
        const bool room = pk->level;
        for (const u32 index : pk->rows)
          if (set.rows[index].derived && set.rows[index].bias != PsoBias::Shadow)
            derived += QueueSetRow(set, index, room ? PsoLane::Derived : PsoLane::Background,
                                   room ? token : nullptr, shadow);
      }
    if (new_offsets) {
      for (const u32 index : casters)
        crossed += QueueSetRow(set, index, PsoLane::Derived, token, shadow);
    } else if (!shadow.empty()) {
      for (const u16 p : fresh)
        if (const PsoSetPackage *pk = package(p))
          for (const u32 index : pk->rows)
            if (set.rows[index].bias == PsoBias::Shadow)
              crossed += QueueSetRow(set, index, PsoLane::Derived, token, shadow);
    }
    for (const u16 p : prefetch)
      if (const PsoSetPackage *pk = package(p))
        for (const u32 index : pk->rows)
          if (set.rows[index].bias != PsoBias::Shadow)
            ahead += QueueSetRow(set, index, PsoLane::Background, nullptr, shadow);
    token->ReleasePending();
    {
      std::lock_guard lock(l.mutex);
      if (captured + derived + crossed)
        l.setsQueued++;
      l.queuedCaptured += captured;
      l.queuedDerived += derived + crossed;
      l.prefetched += ahead;
    }
    const PsoSetPackage *own = package(static_cast<u16>(id));
    EOT_DEBUG("[pso] package {:#x} {}{}: {} resident package(s) new; {} captured, {} derived, {} "
              "caster pipelines queued ({} shadow offset(s){}); {} prefetched for {} room(s){}",
              id, own ? own->name : std::string("?"), level ? " (level)" : "", fresh.size(),
              captured, derived, crossed, shadow.size(), new_offsets ? ", the room's" : "", ahead,
              prefetch.size(), screen ? " (loading screen)" : "");
  });
}

void PsoCacheNoteShaderKey(u64 key, u64 hash, bool pixel) {
  const PsoSet &set = CompiledInSet();
  const auto &map = pixel ? set.psKeyIndex : set.vsKeyIndex;
  auto it = map.find(key);
  if (!hash || it == map.end())
    return;
  std::vector<std::pair<u32, u64>> changed;
  {
    auto &live = liveKeys();
    std::lock_guard lock(live.mutex);
    live.hash.resize(set.keys.size(), 0);
    if (live.hash[it->second] == hash)
      return;
    changed.emplace_back(it->second, live.hash[it->second]);
    live.hash[it->second] = hash;
  }
  PostSetJob([hash, pixel, changed = std::move(changed)] {
    const PsoSet &set = CompiledInSet();
    auto &l = loading();
    std::unordered_set<u16> resident;
    std::vector<PsoShadowValue> shadow;
    {
      std::lock_guard lock(l.mutex);
      resident = l.resident;
      shadow = l.shadowValues;
    }
    u32 queued = 0, rows = 0;
    for (const auto &[k, before] : changed) {
      for (const u32 index : set.keyRows[k]) {
        const PsoSetRow &row = set.rows[index];
        const u64 own = pixel ? row.psHash : row.vsHash;
        if ((before ? before : own) == hash)
          continue;
        bool live_row = false;
        for (const u16 owner : set.rowOwners[index])
          live_row |= owner == 0 || resident.count(owner) != 0;
        if (!live_row)
          continue;
        ++rows;
        queued += QueueSetRow(set, index, row.derived ? PsoLane::Derived : PsoLane::Recorded, nullptr,
                              shadow);
      }
    }
    if (queued)
      EOT_DEBUG("[pso] {} {:016x} is now its key's live variant: {} resident row(s) re-queued, {} "
                "pipelines",
                pixel ? "ps" : "vs", hash, rows, queued);
    std::lock_guard lock(l.mutex);
    l.queuedVariants += queued;
  });
}

void PsoCacheNoteShadowBias(float offset, float slope) {
  if (!std::isfinite(offset) || !std::isfinite(slope) || (offset == 0.0f && slope == 0.0f))
    return;
  const PsoShadowValue v{PolygonOffsetUnits(offset), slope};
  auto &l = loading();
  std::vector<u32> casters;
  {
    std::lock_guard lock(l.mutex);
    if (std::find(l.shadowValues.begin(), l.shadowValues.end(), v) != l.shadowValues.end())
      return;
    l.shadowValues.push_back(v);
    casters = l.casterRows;
  }
  PostSetJob([v, casters = std::move(casters)] {
    const PsoSet &set = CompiledInSet();
    u32 queued = 0;
    for (const u32 index : casters)
      queued += QueueSetRow(set, index, PsoLane::Derived, nullptr, {v});
    auto &l = loading();
    {
      std::lock_guard lock(l.mutex);
      l.queuedDerived += queued;
    }
    EOT_DEBUG("[pso] shadow camera offset {} slope {:g}: {} caster row(s) crossed, {} queued",
              v.depthBias, v.slope, casters.size(), queued);
  });
}

struct MotionVectorsWatch {
  MotionVectorsWatch() {
    rex::cvar::RegisterChangeCallback("eot_motion_vectors", [](std::string_view, std::string_view value) {
      if (value != "true" && value != "1")
        return;
      PostSetJob([] {
        if (!Settings::MotionVectors())
          return;
        auto &l = loading();
        std::vector<u16> resident;
        {
          std::lock_guard lock(l.mutex);
          resident.assign(l.resident.begin(), l.resident.end());
        }
        const PsoSet &set = CompiledInSet();
        std::vector<u32> rows(set.boot);
        for (const u16 p : resident)
          if (auto it = set.packages.find(p); it != set.packages.end())
            rows.insert(rows.end(), it->second.rows.begin(), it->second.rows.end());
        u32 queued = 0;
        PsoRecord r;
        for (const u32 index : rows) {
          const PsoSetRow &row = set.rows[index];
          const i32 velocity = set.velocityOf[row.core];
          if (velocity < 0 || row.bias == PsoBias::Shadow ||
              !SetRecord(set, row, nullptr, &r, velocity))
            continue;
          queued += QueueRecord(r, row.derived ? PsoSource::Derived : PsoSource::CompiledIn,
                                row.derived ? PsoLane::Background : PsoLane::Recorded, nullptr,
                                row.msaa);
        }
        EOT_INFO("[pso] motion vectors on: {} motion-vector pipelines queued for {} resident "
                 "package(s)",
                 queued, resident.size());
      });
    });
  }
} g_motion_vectors_watch;

bool PsoCacheHoldPackage(u32 id) {
  auto &l = loading();
  if (!PsoCacheWaitsAllowed())
    return false;
  std::lock_guard lock(l.mutex);
  auto it = l.holds.find(id);
  if (it == l.holds.end())
    return false;
  PackageHold &h = it->second;
  const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - h.start)
                     .count();
  const TokenPtr screen = PsoPrecacheScreenToken();
  const u32 own = h.token ? h.token->Pending() : 0;
  const u32 pending = own + (screen ? screen->Pending() : 0);
  if (pending == 0 || ms >= kPsoHoldMaxMs) {
    if (h.held) {
      l.holdMs += ms;
      EOT_INFO("[pso] package {:#x} released after {:.0f} ms{}", id, ms,
               pending ? std::format(" (bounded, {} still building)", pending) : "");
    }
    l.holds.erase(it);
    return false;
  }
  if (!h.held) {
    h.held = true;
    l.holdsCount++;
    EOT_INFO("[pso] package {:#x} reported loading: holding the screen for {} pipelines ({} its "
             "own)",
             id, pending, own);
  }
  return true;
}

void PipelineCacheCounts(u32 *alive, u32 *used) {
  auto &c = cache();
  u32 a = 0, u = 0;
  {
    std::shared_lock lock(c.mutex);
    for (const auto &[key, e] : c.map) {
      if (!e.pipeline)
        continue;
      a++;
      u += e.used ? 1 : 0;
    }
  }
  *alive = a;
  *used = u;
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
    std::unique_lock lock(c.mutex);
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
  if (total == 0 && ps.queued == 0)
    return;
  u32 screens, holds, sets_queued, drawn, resident, casters, offsets, prefetched, q_captured,
      q_derived, q_variants;
  f64 hold_ms;
  {
    auto &l = loading();
    std::lock_guard lock(l.mutex);
    screens = l.screens;
    holds = l.holdsCount;
    sets_queued = l.setsQueued;
    hold_ms = l.holdMs;
    drawn = l.drawnCaptured;
    resident = static_cast<u32>(l.resident.size());
    casters = static_cast<u32>(l.casterRows.size());
    offsets = static_cast<u32>(l.shadowValues.size());
    prefetched = l.prefetched;
    q_captured = l.queuedCaptured;
    q_derived = l.queuedDerived;
    q_variants = l.queuedVariants;
  }
  EOT_INFO("[pso] {} pipelines: draw {} | captured {} ({} used) | local {} ({} used) | derived {} "
           "({} used) | render-thread builds since last: {} gaps, {} races | pool: {} queued, {} "
           "built, {} existing, {} skipped, {} failed, pending captured {} derived {} prefetch {} | "
           "set: {} packages resident, {} captured + {} derived queued, {} prefetched, {} for live "
           "shader variants, {} casters x {} shadow offsets | loading: {} screens, {} package sets, "
           "{} holds {:.0f} ms | {} drawn captured",
           total, by_source[0], by_source[1], used_by_source[1], by_source[2], used_by_source[2],
           by_source[3], used_by_source[3], gaps, races, ps.queued, ps.built, ps.existing,
           ps.skipped, ps.failed, ps.recordedPending, ps.derivedPending, ps.backgroundPending,
           resident, q_captured, q_derived, prefetched, q_variants, casters, offsets, screens,
           sets_queued, holds, hold_ms, drawn);
}

}
