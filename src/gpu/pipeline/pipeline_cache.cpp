#include "gpu/pipeline/pipeline_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <xxhash.h>
#include <condition_variable>
#include <deque>
#include <optional>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "core/logging.h"
#include "core/profiling.h"

#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/pipeline/pso_assets.h"
#include "gpu/pipeline/pso_records.h"
#include "gpu/settings.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/vertex_layout.h"

namespace eot::gpu {

namespace {

using Clock = std::chrono::steady_clock;

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
  std::atomic<u32> deferredDraws{0};
  u64 lastSummaryFrame = 0;
};

Cache &cache() {
  static Cache c;
  return c;
}

struct Hold {
  Clock::time_point start;
  PsoPending own;
  bool held = false;
};

struct Loading {
  std::mutex mutex;
  std::atomic<bool> screen{false};
  std::atomic<u32> currentPackage{0};
  std::atomic<u32> levelPackage{0};
  std::unordered_set<u16> loaded;
  std::unordered_map<u16, Clock::time_point> departed;
  std::unordered_map<u32, Hold> holds;
  std::unordered_map<u64, std::vector<u16>> owners;
  std::unordered_set<u64> pinned;
  Clock::time_point lastEvictCheck{};
  u64 screenSinceFrame = 0;
  u32 screens = 0, holdsCount = 0, drawnCaptured = 0, evicted = 0;
  u32 bootQueued = 0, packageQueued = 0;
  f64 holdMs = 0;
};

Loading &loading() {
  static Loading l;
  return l;
}

const char *SourceName(PsoSource s) {
  switch (s) {
  case PsoSource::Recorded:
    return "recorded";
  case PsoSource::Local:
    return "local";
  case PsoSource::Asset:
    return "asset";
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
  if (s.dynamic_depth_bias && st.depthEnable) {
    r.state.depthBias = s.draw_bias_units;
    r.state.slopeScaledDepthBias = s.draw_bias_slope;
    r.state.targetScale = s.draw_bias_scale;
  }
  if (s.host_msaa_samples > 1)
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
  ld.drawnCaptured++;
}

void Own(u64 key, u16 owner) {
  auto &l = loading();
  std::lock_guard lock(l.mutex);
  if (!owner) {
    l.pinned.insert(key);
    l.owners.erase(key);
    return;
  }
  if (l.pinned.count(key))
    return;
  auto &v = l.owners[key];
  if (std::find(v.begin(), v.end(), owner) == v.end())
    v.push_back(owner);
}

u32 Queue(const PsoRecord &r, PsoSource source, PsoLane lane, u16 owner,
          const PsoPending &own = nullptr) {
  u32 n = 0;
  const u32 samples = state().host_msaa_samples;
  const bool multi = samples > 1 && r.state.sampleCount == 1;
  if (multi && (r.msaa == 0 || (r.msaa & kPsoMsaaMulti))) {
    PsoRecord t = r;
    t.state.sampleCount = samples;
    n += PsoPrecacheEnqueue(t, source, lane, own) ? 1 : 0;
    Own(HashPipelineState(t.state), owner);
  }
  if (!multi || r.msaa == 0 || (r.msaa & kPsoMsaaSingle)) {
    n += PsoPrecacheEnqueue(r, source, lane, own) ? 1 : 0;
    Own(HashPipelineState(r.state), owner);
  }
  return n;
}

u32 QueueListRow(u32 row, PsoLane lane, u16 owner, const PsoPending &own = nullptr) {
  const PsoList &list = PsoListGet();
  if (row >= list.rows.size())
    return 0;
  return Queue(list.rows[row], list.sources[row], lane, owner, own);
}

void EvictDeparted() {
  auto &l = loading();
  const auto now = Clock::now();
  std::vector<u64> keys;
  {
    std::lock_guard lock(l.mutex);
    if (now - l.lastEvictCheck < std::chrono::seconds(10) || l.departed.empty())
      return;
    l.lastEvictCheck = now;
    bool expired = false;
    for (auto it = l.departed.begin(); it != l.departed.end();) {
      if (now - it->second >= std::chrono::milliseconds(kPsoEvictAfterMs)) {
        it = l.departed.erase(it);
        expired = true;
      } else {
        ++it;
      }
    }
    if (!expired)
      return;
    for (const auto &[key, packages] : l.owners) {
      bool held = false;
      for (const u16 p : packages)
        held |= l.loaded.count(p) != 0 || l.departed.count(p) != 0;
      if (!held)
        keys.push_back(key);
    }
  }
  std::vector<std::unique_ptr<plume::RenderPipeline>> released;
  std::vector<u64> forgotten;
  {
    auto &c = cache();
    std::unique_lock lock(c.mutex);
    for (const u64 key : keys) {
      auto it = c.map.find(key);
      if (it == c.map.end() || it->second.used || !it->second.pipeline)
        continue;
      released.push_back(std::move(it->second.pipeline));
      c.map.erase(it);
      forgotten.push_back(key);
    }
  }
  PsoPrecacheForget(forgotten);
  std::lock_guard lock(l.mutex);
  for (const u64 key : keys)
    l.owners.erase(key);
  l.evicted += static_cast<u32>(released.size());
  if (!released.empty())
    EOT_DEBUG("[pso] {} pipeline(s) built for packages that have left were never drawn: released",
              released.size());
}

}

bool DynamicDepthBias() { return state().dynamic_depth_bias; }

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
  }
  if (!st.depthEnable || DynamicDepthBias()) {
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
                                           PsoSource source, bool *deferred) {
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
        if (pipeline && c.capture)
          CaptureLocked(s, st);
      }
      if (hot_entry && pipeline)
        *hot_entry = {key, pipeline};
      return pipeline;
    }
  }
  if (deferred && Settings::AsyncPipelines()) {
    const bool race = PsoPrecacheKnown(key);
    if (PsoPrecacheBuildNow(st, key)) {
      std::unique_lock lock(c.mutex);
      (race ? c.raceBuilds : c.gapBuilds)++;
    }
    c.deferredDraws.fetch_add(1, std::memory_order_relaxed);
    *deferred = true;
    return nullptr;
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
  desc.dynamicDepthBiasEnabled = s.dynamic_depth_bias;
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
    const bool race = PsoPrecacheKnown(key);
    (race ? c.raceBuilds : c.gapBuilds)++;
    EOT_DEBUG("[pso] render-thread build ({}) key={:016x} vs={:016x} ps={:016x} spec={:#x} rt0fmt={} "
              "ds={} topo={} package={:#x}{}",
              race ? "still queued" : "not in the list", key, st.vsHash, st.psHash, st.spec,
              static_cast<u32>(st.rtFormats[0]), static_cast<u32>(st.dsFormat),
              static_cast<u32>(st.topology), loading().currentPackage.load(std::memory_order_relaxed),
              loading().screen.load(std::memory_order_relaxed) ? " (loading screen)" : "");
    if (c.capture)
      CaptureLocked(s, st);
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
  if (ps_entry)
    spec_mask |= ps_entry->specConstantsMask;
  CanonicalizePipelineState(st, spec_mask, st.layout->streamMask);
  return GetOrCreatePipeline(s, st, true, source) ? PsoBuildResult::Built : PsoBuildResult::Failed;
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
  PsoListLoad();
  const PsoList &list = PsoListGet();
  const PsoPending boot_rows = std::make_shared<std::atomic<u32>>(0);
  PsoPrecacheBoot(boot_rows);
  u32 queued = 0;
  for (const u32 row : list.boot)
    queued += QueueListRow(row, PsoLane::Load, 0, boot_rows);
  auto &l = loading();
  {
    std::lock_guard lock(l.mutex);
    l.bootQueued = queued;
  }
  EOT_INFO("[pso] boot: {} pipelines queued from {} boot rows; polygon offset {}; drawn pipelines "
           "captured to {}/ as '{}'",
           queued, list.boot.size(), s.dynamic_depth_bias ? "set per draw" : "baked into pipelines",
           kPsoDir, PsoSessionTag());
}

void PsoCacheSetLoadingScreen(bool on) {
  auto &l = loading();
  auto &s = state();
  if (l.screen.exchange(on, std::memory_order_acq_rel) == on)
    return;
  const u32 pending = PsoPrecacheScreenPending();
  PsoPrecacheSetLoading(on);
  std::lock_guard lock(l.mutex);
  if (on) {
    l.screenSinceFrame = s.guest_frames;
    l.screens++;
    EOT_INFO("[pso] loading screen up at frame {}", s.guest_frames);
    return;
  }
  l.holds.clear();
  EOT_INFO("[pso] loading screen down at frame {} after {} frames ({} of its pipelines still building)",
           s.guest_frames, s.guest_frames - l.screenSinceFrame, pending);
}

bool PsoCacheInLoadingScreen() { return loading().screen.load(std::memory_order_acquire); }

void PsoCacheOnPackageLoad(u32 id, bool level) {
  auto &l = loading();
  if (id == 0 || id >= 0x1000)
    return;
  l.currentPackage.store(id, std::memory_order_relaxed);
  if (level)
    l.levelPackage.store(id, std::memory_order_relaxed);
  PsoPending own;
  {
    std::lock_guard lock(l.mutex);
    const bool fresh = l.loaded.insert(static_cast<u16>(id)).second;
    l.departed.erase(static_cast<u16>(id));
    if (fresh && l.screen.load(std::memory_order_acquire)) {
      own = std::make_shared<std::atomic<u32>>(0);
      l.holds[id] = Hold{Clock::now(), own, false};
    }
  }
  const PsoList &list = PsoListGet();
  u32 queued = 0;
  if (auto it = list.byPackage.find(static_cast<u16>(id)); it != list.byPackage.end())
    for (const u32 row : it->second)
      queued += QueueListRow(row, PsoLane::Load, static_cast<u16>(id), own);
  std::lock_guard lock(l.mutex);
  l.packageQueued += queued;
  if (queued)
    EOT_DEBUG("[pso] package {:#x}{}: {} recorded pipelines queued", id, level ? " (level)" : "",
              queued);
}

void PsoCacheOnPackageUnload(u32 id) {
  auto &l = loading();
  if (id == 0 || id >= 0x1000)
    return;
  std::lock_guard lock(l.mutex);
  if (!l.loaded.erase(static_cast<u16>(id)))
    return;
  l.holds.erase(id);
  l.departed[static_cast<u16>(id)] = Clock::now();
}

bool PsoCacheHoldPackage(u32 id) {
  auto &l = loading();
  if (!l.screen.load(std::memory_order_acquire))
    return false;
  std::lock_guard lock(l.mutex);
  auto it = l.holds.find(id);
  if (it == l.holds.end())
    return false;
  Hold &h = it->second;
  const f64 ms = std::chrono::duration<f64, std::milli>(Clock::now() - h.start).count();
  const u32 own = h.own ? h.own->load(std::memory_order_acquire) : 0;
  const u32 pending = own + PsoPrecacheScreenPending();
  if (pending == 0 || ms >= Settings::PsoHoldMaxMs()) {
    if (h.held) {
      l.holdMs += ms;
      EOT_INFO("[pso] package {:#x} released after {:.0f} ms{}", id, ms,
               pending ? std::format(" ({} still building)", pending) : "");
    }
    l.holds.erase(it);
    return false;
  }
  if (!h.held) {
    h.held = true;
    l.holdsCount++;
    EOT_INFO("[pso] package {:#x} loaded: holding the screen for {} pipelines ({} its own)", id,
             pending, own);
  }
  return true;
}

u32 PsoCacheQueue(const PsoRecord &r, PsoSource source, bool background) {
  return Queue(r, source, background ? PsoLane::Background : PsoLane::Load, 0);
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
  if (!force)
    EvictDeparted();
  auto &c = cache();
  if (!force && s.guest_frames < c.lastSummaryFrame + 600)
    return;
  u32 by_source[4] = {}, used_by_source[4] = {}, total = 0, used = 0, gaps, races;
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
      used += e.used ? 1 : 0;
    }
    gaps = c.gapBuilds;
    races = c.raceBuilds;
    c.gapBuilds = c.raceBuilds = 0;
  }
  const u32 waited = c.deferredDraws.exchange(0, std::memory_order_relaxed);
  const PsoPrecacheStats ps = PsoPrecacheGetStats();
  if (total == 0 && ps.queued == 0)
    return;
  const PsoAssetStats as = PsoAssetsGetStats();
  u32 boot, packages, screens, holds, drawn, evicted;
  f64 hold_ms;
  {
    auto &l = loading();
    std::lock_guard lock(l.mutex);
    boot = l.bootQueued;
    packages = l.packageQueued;
    screens = l.screens;
    holds = l.holdsCount;
    hold_ms = l.holdMs;
    drawn = l.drawnCaptured;
    evicted = l.evicted;
  }
  EOT_DEBUG("[pso] {} pipelines, {} drawn: recorded {} ({} drawn), local {} ({}), asset {} ({}), draw {} | "
            "misses since last: {} not in the list, {} still queued, {} draws waited | queued: boot {}, "
            "packages {}, assets {} ({} materials, {} models, {} bundles; {} slots: {} recorded, {} "
            "new layouts, {} alpha-test variants, {} unknown pairs) | pool: {} built, {} for draws, {} skipped, {} failed, pending {} "
            "screen {} loading {} background | loading: {} screens, {} holds {:.0f} ms | {} unused released | "
            "{} drawn captured",
            total, used, by_source[1], used_by_source[1], by_source[2], used_by_source[2], by_source[3],
            used_by_source[3], by_source[0], gaps, races, waited, boot, packages, as.queued,
            as.materials, as.models, as.bundles, as.slots, as.recorded, as.layouts, as.alpha, as.unknown,
            ps.built,
            ps.urgentBuilt, ps.skipped, ps.failed, ps.screenPending, ps.loadPending, ps.backgroundPending,
            screens, holds,
            hold_ms, evicted, drawn);
}

}

namespace eot::gpu {

namespace {

struct WorkItem {
  PsoRecord rec;
  PsoSource source = PsoSource::Draw;
  PsoPending own, screen;
};

struct UrgentItem {
  u64 key = 0;
  PipelineState st;
};

struct Pool {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<UrgentItem> urgent;
  std::unordered_set<u64> urgentKeys;
  std::deque<WorkItem> lanes[3];
  std::unordered_set<u64> known;
  std::vector<std::thread> threads;
  bool started = false, stop = false;
  std::atomic<bool> loading{false};
  std::atomic<bool> boot{false};
  PsoPending bootRows;
  PsoPending screenPending;
  std::atomic<u32> queued{0}, built{0}, existing{0}, skipped{0}, failed{0}, urgentBuilt{0};
};

Pool &pool() {
  static Pool p;
  return p;
}

void SetWorkerPriority(bool loading) {
#if defined(_WIN32)
  ::SetThreadPriority(::GetCurrentThread(),
                      loading ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
  static thread_local bool niced = false;
  if (!niced) {
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 5);
    niced = true;
  }
  sched_param param{};
  param.sched_priority = 0;
  ::pthread_setschedparam(::pthread_self(), loading ? SCHED_OTHER : SCHED_IDLE, &param);
#elif defined(__APPLE__)
  ::pthread_set_qos_class_self_np(loading ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY, 0);
#else
  (void)loading;
#endif
}

void Build(const WorkItem &item) {
  auto &p = pool();
  switch (BuildPipelineFromRecord(state(), item.rec, item.source)) {
  case PsoBuildResult::Built:
    p.built++;
    break;
  case PsoBuildResult::Existing:
    p.existing++;
    break;
  case PsoBuildResult::Skipped:
    p.skipped++;
    break;
  case PsoBuildResult::Failed:
    p.failed++;
    break;
  }
}

void WorkerLoop() {
  auto &p = pool();
  bool raised = false;
  SetWorkerPriority(raised);
  for (;;) {
    std::optional<WorkItem> item;
    std::optional<UrgentItem> urgent;
    {
      std::unique_lock lock(p.mutex);
      const auto idle = [&] {
        return p.urgent.empty() && p.lanes[0].empty() && p.lanes[1].empty() && p.lanes[2].empty();
      };
      p.cv.wait(lock, [&] { return p.stop || !idle(); });
      if (p.stop && idle())
        return;
      if (!p.urgent.empty()) {
        urgent = std::move(p.urgent.front());
        p.urgent.pop_front();
      } else {
        for (auto &lane : p.lanes) {
          if (!lane.empty()) {
            item = std::move(lane.front());
            lane.pop_front();
            break;
          }
        }
      }
    }
    const bool raise = urgent || p.loading.load(std::memory_order_relaxed) ||
                       p.boot.load(std::memory_order_relaxed);
    if (raise != raised) {
      raised = raise;
      SetWorkerPriority(raise);
    }
    if (urgent) {
      GetOrCreatePipeline(state(), urgent->st, true, PsoSource::Draw);
      p.urgentBuilt++;
      std::lock_guard lock(p.mutex);
      p.urgentKeys.erase(urgent->key);
      continue;
    }
    Build(*item);
    if (item->own)
      item->own->fetch_sub(1, std::memory_order_acq_rel);
    if (p.boot.load(std::memory_order_relaxed)) {
      std::lock_guard lock(p.mutex);
      if (p.bootRows && p.bootRows->load(std::memory_order_acquire) == 0 && p.boot.exchange(false)) {
        p.bootRows.reset();
        EOT_INFO("[pso] boot pipelines built; workers back to background priority");
      }
    }
    if (item->screen)
      item->screen->fetch_sub(1, std::memory_order_acq_rel);
  }
}

}

void PsoPrecacheStart() {
  auto &p = pool();
  std::lock_guard lock(p.mutex);
  if (p.started)
    return;
  p.started = true;
  p.stop = false;
  const u32 physical = PhysicalCoreCount();
  const u32 count = std::clamp(physical > 2 ? physical - 2 : 1u, kPsoMinThreads, kPsoMaxThreads);
  for (u32 i = 0; i < count; ++i)
    p.threads.emplace_back(WorkerLoop);
  EOT_INFO("[pso] {} pipeline worker thread(s) for {} physical cores", count, physical);
}

void PsoPrecacheStop() {
  auto &p = pool();
  std::vector<std::thread> threads;
  {
    std::lock_guard lock(p.mutex);
    if (!p.started)
      return;
    p.stop = true;
    p.urgent.clear();
    p.urgentKeys.clear();
    for (auto &lane : p.lanes) {
      for (WorkItem &item : lane) {
        if (item.own)
          item.own->fetch_sub(1, std::memory_order_acq_rel);
        if (item.screen)
          item.screen->fetch_sub(1, std::memory_order_acq_rel);
      }
      lane.clear();
    }
    threads.swap(p.threads);
  }
  p.cv.notify_all();
  for (auto &t : threads)
    if (t.joinable())
      t.join();
  std::lock_guard lock(p.mutex);
  p.started = false;
}

void PsoPrecacheBoot(const PsoPending &boot_rows) {
  auto &p = pool();
  std::lock_guard lock(p.mutex);
  p.bootRows = boot_rows;
  p.boot.store(true, std::memory_order_relaxed);
}

void PsoPrecacheSetLoading(bool loading) {
  auto &p = pool();
  std::lock_guard lock(p.mutex);
  p.loading.store(loading, std::memory_order_relaxed);
  p.screenPending = loading ? std::make_shared<std::atomic<u32>>(0) : nullptr;
}

bool PsoPrecacheEnqueue(const PsoRecord &rec, PsoSource source, PsoLane lane, const PsoPending &own) {
  auto &p = pool();
  const u64 key = HashPipelineState(rec.state);
  PsoPrecacheStart();
  {
    std::lock_guard lock(p.mutex);
    if (p.stop || !p.known.insert(key).second)
      return false;
    const bool screen = lane == PsoLane::Load && p.screenPending;
    if (screen)
      p.screenPending->fetch_add(1, std::memory_order_acq_rel);
    if (own)
      own->fetch_add(1, std::memory_order_acq_rel);
    p.lanes[screen ? 0 : lane == PsoLane::Load ? 1 : 2].push_back(
        WorkItem{rec, source, own, screen ? p.screenPending : nullptr});
  }
  p.queued++;
  p.cv.notify_one();
  return true;
}

bool PsoPrecacheBuildNow(const PipelineState &st, u64 key) {
  auto &p = pool();
  PsoPrecacheStart();
  {
    std::lock_guard lock(p.mutex);
    if (p.stop || !p.urgentKeys.insert(key).second)
      return false;
    p.urgent.push_back(UrgentItem{key, st});
  }
  p.cv.notify_one();
  return true;
}

bool PsoPrecacheKnown(u64 key) {
  auto &p = pool();
  std::lock_guard lock(p.mutex);
  return p.known.count(key) != 0;
}

void PsoPrecacheForget(const std::vector<u64> &keys) {
  auto &p = pool();
  std::lock_guard lock(p.mutex);
  for (const u64 key : keys)
    p.known.erase(key);
}

u32 PsoPrecacheScreenPending() {
  auto &p = pool();
  std::lock_guard lock(p.mutex);
  return p.screenPending ? p.screenPending->load(std::memory_order_acquire) : 0;
}

PsoPrecacheStats PsoPrecacheGetStats() {
  auto &p = pool();
  PsoPrecacheStats st;
  st.queued = p.queued.load();
  st.built = p.built.load();
  st.existing = p.existing.load();
  st.skipped = p.skipped.load();
  st.failed = p.failed.load();
  st.urgentBuilt = p.urgentBuilt.load();
  std::lock_guard lock(p.mutex);
  st.urgentPending = static_cast<u32>(p.urgent.size());
  st.screenPending = static_cast<u32>(p.lanes[0].size());
  st.loadPending = static_cast<u32>(p.lanes[1].size());
  st.backgroundPending = static_cast<u32>(p.lanes[2].size());
  st.threads = static_cast<u32>(p.threads.size());
  return st;
}

}
