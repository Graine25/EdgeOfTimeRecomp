#include "gpu/pipeline/pipeline_cache.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/vertex_layout.h"

namespace eot::gpu {

namespace {

struct Cache {
  std::unordered_map<u64, std::unique_ptr<plume::RenderPipeline>> map;
  u32 failures = 0;
};

Cache &cache() {
  static Cache c;
  return c;
}

}

void ZeroPipelineState(PipelineState &state) { std::memset(&state, 0, sizeof(state)); }

u64 HashPipelineState(const PipelineState &state) {
  return XXH3_64bits(&state, sizeof(state));
}

plume::RenderPipeline *GetOrCreatePipeline(VideoState &s, const PipelineState &st) {
  auto &c = cache();
  const u64 key = HashPipelineState(st);
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.get();
  PerfScope perf_scope(s.perf.pso_ms);
  s.perf.psos++;

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
  if (!pso) {
    if (c.failures++ < 32) {
      EOT_ERROR("[pso] creation failed: vs={} ps={} elements={} rt={} ds={} topo={}",
                static_cast<const void *>(st.vs), static_cast<const void *>(st.ps),
                st.layout ? st.layout->elements.size() : 0, st.rtCount,
                static_cast<u32>(st.dsFormat), static_cast<u32>(st.topology));
    }
    c.map.emplace(key, nullptr);
    return nullptr;
  }
  auto *raw = pso.get();
  static u32 created = 0;
  if (created++ < 400) {
    EOT_INFO("[pso] #{} key={:016x} pso={} vs={} ps={} depth={}{} func{} stencil={} sfunc{} "
             "ops{}/{}/{} cull={} front={} rt0fmt={} ds={} topo={}",
             created, key, static_cast<const void *>(raw), static_cast<const void *>(st.vs),
             static_cast<const void *>(st.ps), st.depthEnable ? "on" : "off",
             st.depthWrite ? "w" : "", static_cast<u32>(st.depthFunc), st.stencilEnable,
             static_cast<u32>(st.stencilFront.compareFunction),
             static_cast<u32>(st.stencilFront.failOp), static_cast<u32>(st.stencilFront.passOp),
             static_cast<u32>(st.stencilFront.depthFailOp), static_cast<u32>(st.cull),
             static_cast<u32>(st.frontFace), static_cast<u32>(st.rtFormats[0]),
             static_cast<u32>(st.dsFormat), static_cast<u32>(st.topology));
  }
  c.map.emplace(key, std::move(pso));
  return raw;
}

}
