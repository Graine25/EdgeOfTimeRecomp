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
#include "src/goliath_engine/gpu/renderer/shaders.h"
#include "src/goliath_engine/gpu/renderer/texture.h"
#include "src/goliath_engine/gpu/renderer/vertex_decl.h"
#include "src/goliath_engine/gpu/renderer/video.h"
#include "src/goliath_engine/kernel/guest_memory.h"

static constexpr uint32_t kVsConstOffset = 0x780;
static constexpr uint32_t kVsConstBytes = 256 * 16;
static constexpr uint32_t kPsConstOffset = 0x1780;
static constexpr uint32_t kPsConstBytes = 224 * 16;
static constexpr uint32_t kSharedBytes = 912;
static constexpr uint32_t kDeclHandleOffset = 12216;
static constexpr float kGameWidth = 1280.0f;
static constexpr float kGameHeight = 720.0f;

namespace gmem = eot::kernel::memory;
using namespace plume;

namespace {

std::atomic<uint32_t> g_s0Base{0}, g_s0Offset{0}, g_s0Stride{0};
std::atomic<uint32_t> g_ibObject{0};

struct CapturedDraw {
  std::vector<uint8_t> verts;
  uint32_t immediateVA = 0;
  uint32_t stride = 0, vertexCount = 0, prim = 0;
  bool indexed = false;
  std::vector<uint32_t> indices;
  uint32_t texAddr = 0, texD0 = 0, texD1 = 0, texD2 = 0;

  RenderShader* vs = nullptr;
  RenderShader* ps = nullptr;
  std::vector<uint8_t> vsConst;  // b0: 4096B, host-endian
  std::vector<uint8_t> psConst;  // b1: 3584B, host-endian
  std::vector<eot::gpu::DeclElem> decl;
  uint32_t declHash = 0;
  float alphaThreshold = 0.0f;
  bool windowSpace = false;
  struct Slot { uint32_t addr = 0, d0 = 0, d1 = 0, d2 = 0; } slots[16];
};

const char* UsageSemantic(uint8_t usage) {
  switch (usage) {
    case 0: return "POSITION";   case 1: return "BLENDWEIGHT"; case 2: return "BLENDINDICES";
    case 3: return "NORMAL";     case 4: return "PSIZE";       case 5: return "TEXCOORD";
    case 6: return "TANGENT";    case 7: return "BINORMAL";    case 9: return "POSITIONT";
    case 10: return "COLOR";     default: return nullptr;
  }
}

RenderFormat DeclTypeFormat(uint32_t type) {
  switch (type) {
    case 0x2C83A4: return RenderFormat::R32_FLOAT;
    case 0x2C23A5: return RenderFormat::R32G32_FLOAT;
    case 0x2A23B9: return RenderFormat::R32G32B32_FLOAT;
    case 0x1A23A6: return RenderFormat::R32G32B32A32_FLOAT;
    case 0x182886: return RenderFormat::B8G8R8A8_UNORM;
    default: return RenderFormat::UNKNOWN;
  }
}

uint32_t HashDecl(const std::vector<eot::gpu::DeclElem>& d) {
  uint32_t h = 2166136261u;
  for (const auto& e : d) {
    for (uint32_t v : {uint32_t(e.usage), uint32_t(e.usageIndex), e.type, uint32_t(e.offset)}) {
      h = (h ^ v) * 16777619u;
    }
  }
  return h;
}

inline void CaptureBoundTexture(CapturedDraw& d) {
  eot::gpu::GetBoundTexture(0, d.texAddr, d.texD0, d.texD1, d.texD2);
}

std::vector<uint8_t> SnapshotConstants(uint8_t* base, uint32_t va, uint32_t bytes) {
  std::vector<uint8_t> out(bytes);
  for (uint32_t i = 0; i < bytes; i += 4) {
    const uint32_t word = gmem::ReadU32(base, va + i);
    std::memcpy(&out[i], &word, 4);
  }
  return out;
}

void CaptureRealShaderState(CapturedDraw& d, uint8_t* base, uint32_t deviceVA) {
  if (deviceVA < 0x1000) return;
  eot::gpu::ResolveShadersForDraw(base, deviceVA);
  d.vs = eot::gpu::CurrentVertexShader();
  d.ps = eot::gpu::CurrentPixelShader();
  d.windowSpace = eot::gpu::CurrentVsIsWindowSpace();
  if (!d.vs || !d.ps) return;
  d.vsConst = SnapshotConstants(base, deviceVA + kVsConstOffset, kVsConstBytes);
  d.psConst = SnapshotConstants(base, deviceVA + kPsConstOffset, kPsConstBytes);
  for (uint32_t s = 0; s < 16; ++s)
    eot::gpu::GetBoundTexture(s, d.slots[s].addr, d.slots[s].d0, d.slots[s].d1, d.slots[s].d2);
  const uint32_t pDecl = gmem::ReadU32(base, deviceVA + kDeclHandleOffset);
  if (eot::gpu::DeclElementsFor(pDecl, d.decl)) d.declHash = HashDecl(d.decl);
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
  desc.depthTargetFormat = RenderFormat::D32_FLOAT;
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
  desc.depthTargetFormat = RenderFormat::D32_FLOAT;
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

struct RealPsoKey {
  RenderShader* vs;
  RenderShader* ps;
  uint32_t stride;
  uint32_t declHash;
  bool operator==(const RealPsoKey& o) const {
    return vs == o.vs && ps == o.ps && stride == o.stride && declHash == o.declHash;
  }
};
struct RealPsoHash {
  size_t operator()(const RealPsoKey& k) const {
    return (reinterpret_cast<uintptr_t>(k.vs) * 1099511628211ull) ^
           (reinterpret_cast<uintptr_t>(k.ps) * 14695981039346656037ull) ^
           (k.stride * 2654435761u) ^ (static_cast<size_t>(k.declHash) << 13);
  }
};
std::unordered_map<RealPsoKey, std::unique_ptr<RenderPipeline>, RealPsoHash> g_realPsoCache;

RenderPipeline* GetRealPSO(const CapturedDraw& d) {
  RealPsoKey key{d.vs, d.ps, d.stride, d.declHash};
  auto it = g_realPsoCache.find(key);
  if (it != g_realPsoCache.end()) return it->second.get();
  RenderDevice* dev = eot::gpu::Device();
  if (!dev) return nullptr;

  std::vector<RenderInputElement> elems;
  elems.reserve(d.decl.size());
  for (const eot::gpu::DeclElem& e : d.decl) {
    const char* sem = UsageSemantic(e.usage);
    const RenderFormat fmt = DeclTypeFormat(e.type);
    if (!sem || fmt == RenderFormat::UNKNOWN) {
      g_realPsoCache[key] = nullptr;
      return nullptr;
    }
    elems.emplace_back(sem, e.usageIndex, static_cast<uint32_t>(elems.size()), fmt, 0, e.offset);
  }
  if (elems.empty()) {
    g_realPsoCache[key] = nullptr;
    return nullptr;
  }

  RenderInputSlot slot(0, d.stride);
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = eot::gpu::GameLayout();
  desc.vertexShader = d.vs;
  desc.pixelShader = d.ps;
  desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
  desc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
  desc.renderTargetCount = 1;
  desc.depthTargetFormat = RenderFormat::D32_FLOAT;
  if (!d.windowSpace) {
    desc.depthEnabled = true;
    desc.depthWriteEnabled = true;
    desc.depthFunction = RenderComparisonFunction::LESS_EQUAL;
  }
  desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = RenderCullMode::NONE;
  desc.inputElements = elems.data();
  desc.inputElementsCount = static_cast<uint32_t>(elems.size());
  desc.inputSlots = &slot;
  desc.inputSlotsCount = 1;
  std::unique_ptr<RenderPipeline> pso = dev->createGraphicsPipeline(desc);
  RenderPipeline* p = pso.get();
  g_realPsoCache[key] = std::move(pso);
  return p;
}

inline void WriteU32LE(uint8_t* p, uint32_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }
inline void WriteF32LE(uint8_t* p, uint32_t off, float v) { std::memcpy(p + off, &v, 4); }

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

std::vector<uint32_t> ExpandIndexed(uint32_t prim, const std::vector<uint32_t>& in, int32_t baseV) {
  std::vector<uint32_t> out;
  auto V = [&](uint32_t i) { return static_cast<uint32_t>(static_cast<int32_t>(i) + baseV); };
  if (prim == 4) {
    out.reserve(in.size());
    for (uint32_t i = 0; i + 2 < in.size(); i += 3) { out.push_back(V(in[i])); out.push_back(V(in[i+1])); out.push_back(V(in[i+2])); }
  } else if (prim == 5) {
    out.reserve(in.size() * 3);
    uint32_t a = 0, b = 0; int n = 0; bool ccw = false;
    for (uint32_t v : in) {
      if (v == 0xFFFF) { n = 0; continue; }
      if (n >= 2) {
        if (!ccw) { out.push_back(V(a)); out.push_back(V(b)); out.push_back(V(v)); }
        else      { out.push_back(V(b)); out.push_back(V(a)); out.push_back(V(v)); }
        ccw = !ccw;
      }
      a = b; b = v; ++n;
    }
  } else if (prim == 13) {
    out.reserve(in.size() / 4 * 6);
    for (uint32_t i = 0; i + 3 < in.size(); i += 4) {
      out.insert(out.end(), {V(in[i]), V(in[i+1]), V(in[i+2]), V(in[i]), V(in[i+2]), V(in[i+3])});
    }
  }
  return out;
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
    std::vector<uint32_t> idx = d.indexed ? std::move(d.indices) : BuildIndices(d.prim, d.vertexCount);
    if (idx.empty() || d.verts.empty()) continue;

    RenderPipeline* realPso = (d.vs && d.ps && !d.decl.empty()) ? GetRealPSO(d) : nullptr;

    if (realPso && d.windowSpace) {
      for (const eot::gpu::DeclElem& e : d.decl) {
        if (e.usage != 0) continue;
        const uint32_t off = e.offset;
        for (size_t v = 0; v + off + 8 <= d.verts.size(); v += d.stride) {
          float px, py;
          std::memcpy(&px, &d.verts[v + off], 4);
          std::memcpy(&py, &d.verts[v + off + 4], 4);
          px = px * 2.0f / kGameWidth - 1.0f;
          py = 1.0f - py * 2.0f / kGameHeight;
          std::memcpy(&d.verts[v + off], &px, 4);
          std::memcpy(&d.verts[v + off + 4], &py, 4);
        }
        break;
      }
    }

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

    if (realPso) {
      std::unique_ptr<RenderBuffer> vsCb = dev->createBuffer(
          RenderBufferDesc::UploadBuffer(d.vsConst.size(), RenderBufferFlag::CONSTANT));
      if (void* p = vsCb->map()) { std::memcpy(p, d.vsConst.data(), d.vsConst.size()); vsCb->unmap(); }
      std::unique_ptr<RenderBuffer> psCb = dev->createBuffer(
          RenderBufferDesc::UploadBuffer(d.psConst.size(), RenderBufferFlag::CONSTANT));
      if (void* p = psCb->map()) { std::memcpy(p, d.psConst.data(), d.psConst.size()); psCb->unmap(); }

      uint8_t shared[kSharedBytes];
      std::memset(shared, 0, sizeof(shared));
      for (uint32_t s = 0; s < 16; ++s) {
        if (!d.slots[s].addr) continue;
        uint32_t idx = eot::gpu::GetOrCreateTextureIndex(cmd, d.slots[s].addr, d.slots[s].d0,
                                                         d.slots[s].d1, d.slots[s].d2);
        WriteU32LE(shared, s * 4, idx);
        WriteU32LE(shared, 256 + s * 4, 0);
      }
      WriteF32LE(shared, 356, 1.0f / static_cast<float>(w));   // g_HalfPixelOffset.x (c22.y)
      WriteF32LE(shared, 360, -1.0f / static_cast<float>(h));  // g_HalfPixelOffset.y
      WriteF32LE(shared, 364, d.alphaThreshold);               // g_AlphaThreshold (c22.w)
      std::unique_ptr<RenderBuffer> shCb =
          dev->createBuffer(RenderBufferDesc::UploadBuffer(kSharedBytes, RenderBufferFlag::CONSTANT));
      if (void* p = shCb->map()) { std::memcpy(p, shared, kSharedBytes); shCb->unmap(); }

      uint32_t cbvVS = 0, cbvPS = 0, cbvShared = 0;
      eot::gpu::GameCbvIndices(cbvVS, cbvPS, cbvShared);
      cmd->setGraphicsPipelineLayout(eot::gpu::GameLayout());
      cmd->setPipeline(realPso);
      cmd->setGraphicsDescriptorSet(eot::gpu::TextureSet(), 0);
      cmd->setGraphicsDescriptorSet(eot::gpu::Tex3DSet(), 1);
      cmd->setGraphicsDescriptorSet(eot::gpu::TexCubeSet(), 2);
      cmd->setGraphicsDescriptorSet(eot::gpu::SamplerSet(), 3);
      cmd->setGraphicsDescriptorSet(eot::gpu::Tex1DSet(), 4);
      cmd->setGraphicsRootDescriptor(vsCb->at(0), cbvVS);
      cmd->setGraphicsRootDescriptor(psCb->at(0), cbvPS);
      cmd->setGraphicsRootDescriptor(shCb->at(0), cbvShared);
      g_frameBuffers.push_back(std::move(vsCb));
      g_frameBuffers.push_back(std::move(psCb));
      g_frameBuffers.push_back(std::move(shCb));
    } else if (texIndex != 0) {
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
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
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
  CaptureRealShaderState(d, base, device);
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
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  __imp__D3DDevice_BeginVertices(ctx, base);
  const uint32_t ptr = ctx.r3.u32;
  if (ptr < 0x1000 || !stride || count == 0 || count > 200000) return;
  CapturedDraw d;
  d.immediateVA = ptr;
  d.stride = stride;
  d.vertexCount = count;
  d.prim = prim;
  CaptureBoundTexture(d);
  CaptureRealShaderState(d, base, device);
  std::lock_guard<std::mutex> lock(g_drawMutex);
  if (g_frameDraws.size() < 8192) g_frameDraws.push_back(std::move(d));
}

REX_EXTERN(__imp__D3DDevice_SetIndices);
REX_HOOK_RAW(D3DDevice_SetIndices) {
  g_ibObject.store(ctx.r4.u32, std::memory_order_relaxed);
  __imp__D3DDevice_SetIndices(ctx, base);
}

REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32;
  const int32_t baseVertex = static_cast<int32_t>(ctx.r5.u32);
  const uint32_t startIndex = ctx.r6.u32, indexCount = ctx.r7.u32;
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);

  const uint32_t ib = g_ibObject.load(std::memory_order_relaxed);
  const uint32_t vbBase = g_s0Base.load(std::memory_order_relaxed) & ~3u;
  const uint32_t vbOff = g_s0Offset.load(std::memory_order_relaxed);
  const uint32_t stride = g_s0Stride.load(std::memory_order_relaxed);
  if (ib < 0x1000 || !vbBase || !stride || indexCount == 0 || indexCount > 1000000) return;

  const uint32_t ibAddr = gmem::ReadU32(base, ib + 0x18);
  if (ibAddr < 0x1000) return;

  std::vector<uint32_t> raw(indexCount);
  uint32_t maxIdx = 0;
  for (uint32_t i = 0; i < indexCount; ++i) {
    const uint16_t v = gmem::ReadU16(base, ibAddr + (startIndex + i) * 2);
    raw[i] = v;
    if (v != 0xFFFF && v > maxIdx) maxIdx = v;
  }
  std::vector<uint32_t> tris = ExpandIndexed(prim, raw, baseVertex);
  if (tris.empty()) return;

  const uint32_t vertCount = static_cast<uint32_t>(baseVertex) + maxIdx + 1;
  if (vertCount > 2000000) return;
  const uint32_t bytes = vertCount * stride;
  const uint32_t startVA = vbBase + vbOff;

  CapturedDraw d;
  d.stride = stride;
  d.vertexCount = vertCount;
  d.prim = prim;
  d.indexed = true;
  d.indices = std::move(tris);
  CaptureBoundTexture(d);
  CaptureRealShaderState(d, base, device);
  d.verts.resize(bytes);
  for (uint32_t i = 0; i < bytes; i += 4) {
    const uint32_t word = gmem::ReadU32(base, startVA + i);
    std::memcpy(&d.verts[i], &word, 4);
  }
  std::lock_guard<std::mutex> lock(g_drawMutex);
  if (g_frameDraws.size() < 8192) g_frameDraws.push_back(std::move(d));
}
