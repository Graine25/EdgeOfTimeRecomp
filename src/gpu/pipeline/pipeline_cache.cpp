#include "gpu/pipeline/pipeline_cache.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/hash.h>

#include "core/logging.h"
#include "gpu/device/device.h"
#include "gpu/guest/resources.h"
#include "gpu/shaders/shader_cache.h"

namespace eot::gpu {

namespace {

std::mutex g_pipeline_mutex;

struct Entry {
  std::unique_ptr<plume::RenderPipeline> pipeline;
  u32 draws = 0;
};
std::unordered_map<PipelineKey, Entry, PipelineKeyHash> g_pipelines;

std::atomic<u32> g_lookups{0};
std::atomic<u32> g_undescribable{0};

void LogPipelineStatsLocked();

void Mix(u64 &h, u64 value) {
  h ^= value + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
}

}

size_t PipelineKeyHash::operator()(const PipelineKey &k) const {
  u64 h = 0;
  Mix(h, k.vertexShaderHash);
  Mix(h, k.pixelShaderHash);
  Mix(h, (u64(k.vertexSpecConstants) << 32) | k.pixelSpecConstants);
  Mix(h, (u64(static_cast<u32>(k.renderTargetFormat)) << 32) |
             static_cast<u32>(k.depthFormat));
  Mix(h, (u64(static_cast<u32>(k.topology)) << 32) | k.sampleCount);
  Mix(h, k.stateHash);
  return static_cast<size_t>(h);
}

bool BuildPipelineKeyForCurrentState(PipelineKey &out) {
  GuestShader *vs = Video::BoundVertexShader();
  if (!vs || !vs->shaderCacheEntry)
    return false;

  out = PipelineKey{};
  out.vertexShaderHash = vs->hash;
  out.vertexSpecConstants = 0;

  if (GuestShader *ps = Video::BoundPixelShader()) {
    if (ps->shaderCacheEntry) {
      out.pixelShaderHash = ps->hash;
      out.pixelSpecConstants = 0;
    }
  }

  const Video::AttachmentFormats fmts = Video::BoundAttachmentFormats();
  if (fmts.color == plume::RenderFormat::UNKNOWN &&
      fmts.depth == plume::RenderFormat::UNKNOWN)
    return false;
  out.renderTargetFormat = fmts.color;
  out.depthFormat = fmts.depth;
  out.sampleCount = fmts.sampleCount;
  return true;
}

plume::RenderPipeline *GetOrCreatePipeline(const PipelineKey &key) {
  g_lookups.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(g_pipeline_mutex);
  auto [it, inserted] = g_pipelines.try_emplace(key);
  ++it->second.draws;
  if (inserted && g_pipelines.size() == 1) {
    EOT_INFO("[pso] first key: vs=0x{:016X} ps=0x{:016X} rt={} ds={} topo={}",
             key.vertexShaderHash, key.pixelShaderHash,
             static_cast<u32>(key.renderTargetFormat),
             static_cast<u32>(key.depthFormat),
             static_cast<u32>(key.topology));
  }
  if (inserted) {
    const size_t n = g_pipelines.size();
    if (n == 1 || n == 10 || n == 50 || n % 100 == 0)
      LogPipelineStatsLocked();
  }
  return it->second.pipeline.get();
}

namespace {

void LogPipelineStatsLocked() {
  u32 depth_only = 0;
  for (const auto &[key, entry] : g_pipelines) {
    if (key.pixelShaderHash == 0)
      ++depth_only;
  }
  EOT_INFO("[pso] {} distinct keys from {} lookups ({} depth-only); {} draws "
           "undescribable",
           g_pipelines.size(), g_lookups.load(), depth_only,
           g_undescribable.load());
}

}

void LogPipelineStats() {
  std::lock_guard lock(g_pipeline_mutex);
  LogPipelineStatsLocked();
}

void NotePipelineUndescribable() {
  g_undescribable.fetch_add(1, std::memory_order_relaxed);
}

}
