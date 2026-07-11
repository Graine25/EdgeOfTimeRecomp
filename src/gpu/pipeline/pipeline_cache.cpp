#include "gpu/pipeline/pipeline_cache.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/hash.h>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/settings.h"
#include "gpu/device/device.h"
#include "gpu/guest/resources.h"
#include "gpu/pipeline/vertex_layout.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/shaders/shader_cache.h"

REXCVAR_DEFINE_BOOL(eot_build_guest_pipelines, true, kCvarGroup,
                    "Create host pipelines for guest draws.");

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
std::atomic<u32> g_layout_ok{0};
std::atomic<u32> g_layout_failed{0};
std::atomic<u32> g_layout_reported{0};
std::atomic<u32> g_layout_no_fetches{0};
std::atomic<u32> g_layout_no_decl{0};
std::atomic<u32> g_layout_join_failed{0};
std::atomic<u32> g_built{0};
std::atomic<u32> g_build_failed{0};

void LogPipelineStatsLocked();

void Mix(u64 &h, u64 value) {
  h ^= value + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
}

}

void NoteInputLayout(const InputLayout &layout, bool ok) {
  if (ok) {
    if (g_layout_ok.fetch_add(1, std::memory_order_relaxed) == 0) {
      char buf[320];
      int len = 0;
      for (u32 i = 0; i < layout.count && len < int(sizeof(buf)) - 40; ++i) {
        const auto &e = layout.elements[i];
        len += snprintf(buf + len, sizeof(buf) - len, "%s%s%u@s%u+%u:f%u",
                        i ? " " : "", VertexUsageName(e.usage), e.usageIndex,
                        e.stream, e.offset, static_cast<u32>(e.format));
      }
      EOT_INFO("[pso] first input layout: {} elements - {}", layout.count, buf);
    }
  } else {
    g_layout_failed.fetch_add(1, std::memory_order_relaxed);
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
  Mix(h, k.inputLayoutHash);
  Mix(h, k.stateHash);
  return static_cast<size_t>(h);
}

u64 HashInputLayout(const InputLayout &layout) {
  u64 h = 0;
  for (u32 i = 0; i < layout.count; ++i) {
    const auto &e = layout.elements[i];
    Mix(h, (u64(static_cast<u32>(e.usage)) << 40) | (u64(e.usageIndex) << 32) |
               (u64(e.stream) << 24) | e.offset);
    Mix(h, static_cast<u32>(e.format));
  }
  return h;
}

bool BuildPipelineKeyForCurrentState(u32 device_va, PipelineKey &out) {
  GuestShader *vs = Video::BoundVertexShader();
  if (!vs || !vs->shaderCacheEntry)
    return false;

  out = PipelineKey{};
  out.vertexShaderHash = vs->hash;

  VertexLayout fetches;
  VertexDeclaration decl;
  InputLayout input;
  if (!DecodeVertexLayout(vs, fetches)) {
    g_layout_no_fetches.fetch_add(1, std::memory_order_relaxed);
    NoteInputLayout(input, false);
  } else if (!CurrentVertexDeclaration(device_va, decl)) {
    g_layout_no_decl.fetch_add(1, std::memory_order_relaxed);
    NoteInputLayout(input, false);
  } else if (!BuildInputLayout(fetches, decl, input)) {
    g_layout_join_failed.fetch_add(1, std::memory_order_relaxed);
    NoteInputLayout(input, false);
  } else {
    NoteInputLayout(input, true);
    out.inputLayoutHash = HashInputLayout(input);
    out.layout = input;
  }
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

namespace {

std::unique_ptr<plume::RenderPipeline>
BuildPipeline(const PipelineKey &key, const InputLayout &layout) {
  if (!REXCVAR_GET(eot_build_guest_pipelines))
    return nullptr;
  auto *device = Video::HostDevice();
  GuestShader *vs = Video::BoundVertexShader();
  GuestShader *ps = Video::BoundPixelShader();
  if (!device || !vs)
    return nullptr;

  plume::RenderShader *host_vs = GetOrLinkShader(vs, key.vertexSpecConstants);
  if (!host_vs)
    return nullptr;
  plume::RenderShader *host_ps =
      ps ? GetOrLinkShader(ps, key.pixelSpecConstants) : nullptr;

  plume::RenderInputElement elements[kMaxVertexFetches]{};
  for (u32 i = 0; i < layout.count; ++i) {
    const auto &e = layout.elements[i];
    elements[i] = plume::RenderInputElement(VertexUsageSemantic(e.usage),
                                            e.usageIndex, i,
                                            e.format, e.stream, e.offset);
  }

  plume::RenderInputSlot slots[kMaxStreamSources]{};
  u32 slot_count = 0;
  for (u32 i = 0; i < layout.count; ++i) {
    const u32 stream = layout.elements[i].stream;
    bool seen = false;
    for (u32 j = 0; j < slot_count; ++j)
      seen = seen || slots[j].index == stream;
    if (seen)
      continue;
    const u32 stride = Video::BoundStreamStride(stream);
    if (stride == 0)
      return nullptr;
    slots[slot_count++] = plume::RenderInputSlot(
        stream, stride, plume::RenderInputSlotClassification::PER_VERTEX_DATA);
  }

  plume::RenderPipelineLayout *layout_obj = Video::GuestPipelineLayout();
  if (!layout_obj)
    return nullptr;

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_obj;
  desc.vertexShader = host_vs;
  desc.pixelShader = host_ps;
  desc.inputElements = elements;
  desc.inputElementsCount = layout.count;
  desc.inputSlots = slots;
  desc.inputSlotsCount = slot_count;
  desc.primitiveTopology = key.topology;
  desc.multisampling.sampleCount = key.sampleCount;
  desc.depthTargetFormat = key.depthFormat;
  if (key.renderTargetFormat != plume::RenderFormat::UNKNOWN) {
    desc.renderTargetFormat[0] = key.renderTargetFormat;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
  }
  desc.specConstants = nullptr;
  desc.specConstantsCount = 0;

  return device->createGraphicsPipeline(desc);
}

}

plume::RenderPipeline *GetOrCreatePipeline(const PipelineKey &key,
                                           const InputLayout &layout) {
  g_lookups.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(g_pipeline_mutex);
    auto it = g_pipelines.find(key);
    if (it != g_pipelines.end()) {
      ++it->second.draws;
      return it->second.pipeline.get();
    }
  }

  std::unique_ptr<plume::RenderPipeline> built = BuildPipeline(key, layout);
  if (!built && g_build_failed.fetch_add(1, std::memory_order_relaxed) < 3) {
    EOT_WARN("[pso] build failed: vs=0x{:016X} ps=0x{:016X} rt={} ds={} "
             "{} elements",
             key.vertexShaderHash, key.pixelShaderHash,
             static_cast<u32>(key.renderTargetFormat),
             static_cast<u32>(key.depthFormat), layout.count);
  }

  std::lock_guard lock(g_pipeline_mutex);
  auto [it, inserted] = g_pipelines.try_emplace(key);
  ++it->second.draws;
  if (inserted) {
    it->second.pipeline = std::move(built);
    if (it->second.pipeline)
      g_built.fetch_add(1, std::memory_order_relaxed);
  }
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
  EOT_INFO("[pso] {} distinct keys from {} lookups ({} depth-only); {} "
           "undescribable; layouts {} ok / {} failed; pipelines {} built, {} failed",
           g_pipelines.size(), g_lookups.load(), depth_only,
           g_undescribable.load(), g_layout_ok.load(),
           g_layout_failed.load(), g_built.load(), g_build_failed.load());
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
