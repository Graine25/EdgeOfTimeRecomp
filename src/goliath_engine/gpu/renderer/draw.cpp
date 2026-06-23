#include "draw.h"

#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

#include "src/goliath_engine/gpu/renderer/shader/generated/debug_draw_dxil.h"
#include "src/goliath_engine/gpu/renderer/shader/generated/textured_draw_dxil.h"
#include "src/goliath_engine/gpu/renderer/texture.h"
#include "src/goliath_engine/gpu/renderer/video.h"
#include "src/goliath_engine/kernel/guest_memory.h"

namespace gmem = eot::kernel::memory;
using namespace plume;

namespace {

std::atomic<uint32_t> g_s0Base{0}, g_s0Offset{0}, g_s0Stride{0};

struct CapturedDraw {
  std::vector<uint8_t> verts;
  uint32_t immediateVA = 0;
  uint32_t stride = 0, vertexCount = 0, prim = 0;
  uint32_t texAddr = 0, texD0 = 0, texD1 = 0, texD2 = 0;
};

inline void CaptureBoundTexture(CapturedDraw& d) {
  eot::gpu::GetBoundTexture(0, d.texAddr, d.texD0, d.texD1, d.texD2);
}
std::vector<CapturedDraw> g_frameDraws;
std::mutex g_drawMutex;

std::unique_ptr<RenderShader> g_dbgVS, g_dbgPS;
std::unique_ptr<RenderPipelineLayout> g_layout;
std::unordered_map<uint32_t, std::unique_ptr<RenderPipeline>> g_psoByStride;
std::vector<std::unique_ptr<RenderBuffer>> g_frameBuffers;

bool EnsureReplayResources() {
  RenderDevice* dev = eot::gpu::Device();
  if (!dev) return false;
  if (!g_dbgVS)
    g_dbgVS = dev->createShader(g_debug_vs_dxil, sizeof(g_debug_vs_dxil), "VSMain",
                                RenderShaderFormat::DXIL);
  if (!g_dbgPS)
    g_dbgPS = dev->createShader(g_debug_ps_dxil, sizeof(g_debug_ps_dxil), "PSMain",
                                RenderShaderFormat::DXIL);
  if (!g_layout) {
    RenderPipelineLayoutBuilder lb;
    lb.begin(false, true);
    lb.end();
    g_layout = lb.create(dev);
  }
  return g_dbgVS && g_dbgPS && g_layout;
}

RenderPipeline* GetPSO(uint32_t stride) {
  auto it = g_psoByStride.find(stride);
  if (it != g_psoByStride.end()) return it->second.get();
  RenderDevice* dev = eot::gpu::Device();
  const RenderInputElement elems[] = {
      RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, 0),
  };
  RenderInputSlot slot(0, stride);
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_layout.get();
  desc.vertexShader = g_dbgVS.get();
  desc.pixelShader = g_dbgPS.get();
  desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
  desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
  desc.renderTargetCount = 1;
  desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = RenderCullMode::NONE;
  desc.inputElements = elems;
  desc.inputElementsCount = 1;
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  std::unique_ptr<RenderPipeline> pso = dev->createGraphicsPipeline(desc);
  RenderPipeline* p = pso.get();
  g_psoByStride[stride] = std::move(pso);
  return p;
}

std::unique_ptr<RenderShader> g_txVS, g_txPS;
std::unordered_map<uint32_t, std::unique_ptr<RenderPipeline>> g_txPsoByStride;

RenderPipeline* GetTexturedPSO(uint32_t stride) {
  auto it = g_txPsoByStride.find(stride);
  if (it != g_txPsoByStride.end()) return it->second.get();
  RenderDevice* dev = eot::gpu::Device();
  if (!g_txVS)
    g_txVS = dev->createShader(g_textured_vs_dxil, sizeof(g_textured_vs_dxil), "VSMain",
                               RenderShaderFormat::DXIL);
  if (!g_txPS)
    g_txPS = dev->createShader(g_textured_ps_dxil, sizeof(g_textured_ps_dxil), "PSMain",
                               RenderShaderFormat::DXIL);
  const RenderInputElement elems[] = {
      RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, 0),
      RenderInputElement("COLOR", 0, 1, RenderFormat::B8G8R8A8_UNORM, 0, 8),
      RenderInputElement("TEXCOORD", 0, 2, RenderFormat::R32G32_FLOAT, 0, 12),
  };
  RenderInputSlot slot(0, stride);
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = eot::gpu::TexturedPipelineLayout();
  desc.vertexShader = g_txVS.get();
  desc.pixelShader = g_txPS.get();
  desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
  desc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
  desc.renderTargetCount = 1;
  desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = RenderCullMode::NONE;
  desc.inputElements = elems;
  desc.inputElementsCount = 3;
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  std::unique_ptr<RenderPipeline> pso = dev->createGraphicsPipeline(desc);
  RenderPipeline* p = pso.get();
  g_txPsoByStride[stride] = std::move(pso);
  return p;
}

std::vector<uint32_t> BuildIndices(uint32_t prim, uint32_t vc) {
  std::vector<uint32_t> idx;
  if (prim == 13) {
    const uint32_t quads = vc / 4;
    idx.reserve(quads * 6);
    for (uint32_t q = 0; q < quads; ++q) {
      const uint32_t b = q * 4;
      idx.insert(idx.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
  } else if (prim == 4) {
    idx.reserve(vc);
    for (uint32_t i = 0; i < vc; ++i) idx.push_back(i);
  } else if (prim == 5 && vc >= 3) {
    idx.reserve((vc - 2) * 3);
    for (uint32_t i = 0; i + 2 < vc; ++i) {
      if (i & 1)
        idx.insert(idx.end(), {i + 1, i, i + 2});
      else
        idx.insert(idx.end(), {i, i + 1, i + 2});
    }
  }
  return idx;
}

}

namespace eot::gpu {

void ReplayCapturedDraws(RenderCommandList* cmd, uint32_t w, uint32_t h) {
  if (!cmd || !EnsureReplayResources()) return;
  std::vector<CapturedDraw> draws;
  {
    std::lock_guard<std::mutex> lock(g_drawMutex);
    draws.swap(g_frameDraws);
  }
  if (draws.empty()) return;

  RenderDevice* dev = eot::gpu::Device();
  g_frameBuffers.clear();
  eot::gpu::EnsureTextureSystem();
  eot::gpu::BeginTextureFrame();

  cmd->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)));
  cmd->setScissors(RenderRect(0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)));

  uint8_t* mb = rex::system::kernel_state()->memory()->virtual_membase();
  for (CapturedDraw& d : draws) {
    if (!d.immediateVA || !d.verts.empty()) continue;
    const uint32_t bytes = d.vertexCount * d.stride;
    d.verts.resize(bytes);
    for (uint32_t i = 0; i < bytes; i += 4) {
      const uint32_t word = gmem::ReadU32(mb, d.immediateVA + i);
      std::memcpy(&d.verts[i], &word, 4);
    }
  }

  uint32_t drawn = 0;
  for (CapturedDraw& d : draws) {
    std::vector<uint32_t> idx = BuildIndices(d.prim, d.vertexCount);
    if (idx.empty() || d.verts.empty()) continue;

    std::unique_ptr<RenderBuffer> vb =
        dev->createBuffer(RenderBufferDesc::VertexBuffer(d.verts.size(), RenderHeapType::UPLOAD));
    if (void* p = vb->map()) {
      std::memcpy(p, d.verts.data(), d.verts.size());
      vb->unmap();
    }
    const uint64_t ibBytes = idx.size() * sizeof(uint32_t);
    std::unique_ptr<RenderBuffer> ib =
        dev->createBuffer(RenderBufferDesc::IndexBuffer(ibBytes, RenderHeapType::UPLOAD));
    if (void* p = ib->map()) {
      std::memcpy(p, idx.data(), ibBytes);
      ib->unmap();
    }

    uint32_t texIndex = 0;
    if (d.stride >= 20 && d.texAddr != 0)
      texIndex = eot::gpu::GetOrCreateTextureIndex(cmd, d.texAddr, d.texD0, d.texD1, d.texD2);
    if (texIndex != 0) {
      cmd->setGraphicsPipelineLayout(eot::gpu::TexturedPipelineLayout());
      cmd->setPipeline(GetTexturedPSO(d.stride));
      cmd->setGraphicsDescriptorSet(eot::gpu::TextureSet(), 0);
      cmd->setGraphicsDescriptorSet(eot::gpu::SamplerSet(), 1);
      cmd->setGraphicsPushConstants(0, &texIndex, 0, sizeof(texIndex));
    } else {
      cmd->setGraphicsPipelineLayout(g_layout.get());
      cmd->setPipeline(GetPSO(d.stride));
    }
    RenderVertexBufferView vbv(vb.get(), static_cast<uint32_t>(d.verts.size()));
    RenderInputSlot slot(0, d.stride);
    cmd->setVertexBuffers(0, &vbv, 1, &slot);
    RenderIndexBufferView ibv(ib.get(), static_cast<uint32_t>(ibBytes), RenderFormat::R32_UINT);
    cmd->setIndexBuffer(&ibv);
    cmd->drawIndexedInstanced(static_cast<uint32_t>(idx.size()), 1, 0, 0, 0);

    g_frameBuffers.push_back(std::move(vb));
    g_frameBuffers.push_back(std::move(ib));
    ++drawn;
  }
  static uint64_t s_log = 0;
  if ((s_log++ % 120) == 0)
    REXGPU_INFO("[draw] replayed {} draws ({} captured)", drawn, draws.size());
}

}

REX_EXTERN(__imp__D3DDevice_SetStreamSource);
REX_HOOK_RAW(D3DDevice_SetStreamSource) {
  const uint32_t stream = ctx.r4.u32, vb = ctx.r5.u32, off = ctx.r6.u32, stride = ctx.r7.u32;
  if (stream == 0) {
    const uint32_t base0 = (vb >= 0x1000) ? gmem::ReadU32(base, vb + 0x18) : 0;
    g_s0Base.store(base0, std::memory_order_relaxed);
    g_s0Offset.store(off, std::memory_order_relaxed);
    g_s0Stride.store(stride, std::memory_order_relaxed);
  }
  __imp__D3DDevice_SetStreamSource(ctx, base);
}

REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_HOOK_RAW(D3DDevice_DrawVertices) {
  const uint32_t prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  __imp__D3DDevice_DrawVertices(ctx, base);

  const uint32_t baseAddr = g_s0Base.load(std::memory_order_relaxed) & ~3u;
  const uint32_t off = g_s0Offset.load(std::memory_order_relaxed);
  const uint32_t stride = g_s0Stride.load(std::memory_order_relaxed);
  if (!baseAddr || !stride || count == 0 || count > 200000) return;
  const uint32_t startVA = baseAddr + off + start * stride;
  if (startVA < 0x1000) return;
  const uint32_t bytes = count * stride;

  CapturedDraw d;
  d.stride = stride;
  d.vertexCount = count;
  d.prim = prim;
  CaptureBoundTexture(d);
  d.verts.resize(bytes);
  for (uint32_t i = 0; i < bytes; i += 4) {
    const uint32_t word = gmem::ReadU32(base, startVA + i);
    std::memcpy(&d.verts[i], &word, 4);
  }
  static std::atomic<uint64_t> s_n{0};
  uint64_t nn = s_n.fetch_add(1, std::memory_order_relaxed);
  if (nn < 16) {
    float x, y;
    std::memcpy(&x, &d.verts[0], 4);
    std::memcpy(&y, &d.verts[4], 4);
    REXGPU_INFO("[draw] capture prim={} count={} stride={} va=0x{:08X} v0=({:.2f},{:.2f})", prim,
                count, stride, startVA, x, y);
  }
  std::lock_guard<std::mutex> lock(g_drawMutex);
  if (g_frameDraws.size() < 8192) g_frameDraws.push_back(std::move(d));
}

REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_HOOK_RAW(D3DDevice_BeginVertices) {
  const uint32_t prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  __imp__D3DDevice_BeginVertices(ctx, base);
  const uint32_t ptr = ctx.r3.u32;
  if (ptr < 0x1000 || !stride || count == 0 || count > 200000) return;
  CapturedDraw d;
  d.immediateVA = ptr;
  d.stride = stride;
  d.vertexCount = count;
  d.prim = prim;
  CaptureBoundTexture(d);
  std::lock_guard<std::mutex> lock(g_drawMutex);
  if (g_frameDraws.size() < 8192) g_frameDraws.push_back(std::move(d));
}
