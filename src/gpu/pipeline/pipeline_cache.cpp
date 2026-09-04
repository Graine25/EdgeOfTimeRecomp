#include "gpu/pipeline/pipeline_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
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
#include "gpu/settings.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/vertex_layout.h"

namespace eot::gpu {

namespace {

struct PsoRecord {
  PipelineState state;
  u32 declCount;
  u8 declRaw[32 * sizeof(DeclElement)];
};
constexpr u32 kPsoFileMagic = 0x4F535045;
constexpr u32 kPsoFileVersion = 1;
struct PsoFileHeader {
  u32 magic, version, recordSize, reserved;
};

struct Cache {
  std::mutex mutex;
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> map;
  u32 failures = 0;
  std::unordered_set<u64> recorded;
  std::vector<PsoRecord> pending;
  std::string path;
  bool fileHasHeader = false;
  u64 lastFlushFrame = 0;
  u32 precached = 0;
};

Cache &cache() {
  static Cache c;
  return c;
}

void RecordLocked(Cache &c, u64 key, const PipelineState &st) {
  if (c.path.empty() || !st.layout || c.recorded.count(key))
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
  c.pending.push_back(r);
  c.recorded.insert(key);
}

bool ReadRecords(const std::string &path, std::vector<PsoRecord> &out) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  PsoFileHeader h{};
  bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && h.magic == kPsoFileMagic &&
            h.version == kPsoFileVersion && h.recordSize == sizeof(PsoRecord);
  if (ok) {
    PsoRecord r;
    while (std::fread(&r, sizeof(r), 1, f) == 1)
      out.push_back(r);
  }
  std::fclose(f);
  return ok;
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
    static u32 created = 0;
    if (created++ < 400) {
      EOT_INFO("[pso] #{} key={:016x} pso={} vs={:016x} ps={:016x} spec={:#x} depth={}{} func{} "
               "stencil={} cull={} front={} rt0fmt={} ds={} topo={}",
               created, key, static_cast<const void *>(raw), st.vsHash, st.psHash, st.spec,
               st.depthEnable ? "on" : "off", st.depthWrite ? "w" : "",
               static_cast<u32>(st.depthFunc), st.stencilEnable, static_cast<u32>(st.cull),
               static_cast<u32>(st.frontFace), static_cast<u32>(st.rtFormats[0]),
               static_cast<u32>(st.dsFormat), static_cast<u32>(st.topology));
    }
    RecordLocked(c, key, st);
  }
  c.map.emplace(key, std::move(pso));
  return raw;
}

void PsoCachePrecache() {
  auto &c = cache();
  auto &s = state();
  c.path = Settings::PsoCachePath();
  if (c.path.empty() || !s.ready || !s.device)
    return;
  std::vector<PsoRecord> records;
  if (!ReadRecords(c.path, records)) {
    EOT_INFO("[pso] no usable cache at {}; recording this run", c.path);
    return;
  }
  c.fileHasHeader = true;
  const auto t0 = std::chrono::steady_clock::now();
  for (const PsoRecord &r : records)
    c.recorded.insert(HashPipelineState(r.state));

  std::atomic<size_t> next{0};
  std::atomic<u32> skipped{0};
  auto work = [&]() {
    for (;;) {
      const size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= records.size())
        return;
      const PsoRecord &r = records[i];
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
  const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  EOT_INFO("[pso] precached {} of {} recorded pipelines in {:.0f} ms on {} threads ({} skipped: "
           "shader not in cache or layout mismatch)",
           c.precached, records.size(), ms, threads, skipped.load());
}

void PsoCacheFlushIfDirty(bool force) {
  auto &c = cache();
  auto &s = state();
  std::lock_guard lock(c.mutex);
  if (c.path.empty() || c.pending.empty())
    return;
  if (!force && s.guest_frames < c.lastFlushFrame + 300)
    return;
  c.lastFlushFrame = s.guest_frames;
  FILE *f = std::fopen(c.path.c_str(), c.fileHasHeader ? "ab" : "wb");
  if (!f) {
    EOT_WARN("[pso] cannot open {} for writing; recording disabled", c.path);
    c.path.clear();
    return;
  }
  if (!c.fileHasHeader) {
    const PsoFileHeader h{kPsoFileMagic, kPsoFileVersion, sizeof(PsoRecord), 0};
    std::fwrite(&h, sizeof(h), 1, f);
    c.fileHasHeader = true;
  }
  std::fwrite(c.pending.data(), sizeof(PsoRecord), c.pending.size(), f);
  std::fclose(f);
  EOT_INFO("[pso] recorded {} new pipeline keys to {}", c.pending.size(), c.path);
  c.pending.clear();
}

}
