#include "src/goliath_engine/gpu/renderer/render_state.h"

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

#include "src/goliath_engine/gpu/renderer/guest_device.h"
#include "src/goliath_engine/gpu/renderer/guest_resources.h"
#include "src/goliath_engine/gpu/renderer/render_internal.h"
#include "src/goliath_engine/gpu/renderer/shader/generated/debug_draw_dxil.h"
#include "src/goliath_engine/gpu/renderer/shader/generated/textured_draw_dxil.h"
#include "src/goliath_engine/kernel/guest_memory.h"

namespace gmem = eot::kernel::memory;
using namespace plume;

namespace {

std::atomic<uint32_t> g_s0Base{0}, g_s0Offset{0}, g_s0Stride{0};
std::atomic<uint32_t> g_ibObject{0};

struct BoundTex {
  uint32_t addr = 0;
  eot::render::TextureFetch fetch;
};
BoundTex g_bound[16];
std::mutex g_boundMutex;

struct CapturedDraw {
  std::vector<uint8_t> verts;
  uint32_t immediateVA = 0;
  uint32_t stride = 0, vertexCount = 0, prim = 0;
  bool indexed = false;
  std::vector<uint32_t> indices;
  uint32_t texAddr = 0;
  eot::render::TextureFetch texFetch;

  RenderShader* vs = nullptr;
  RenderShader* ps = nullptr;
  std::vector<uint8_t> vsConst;  // b0: 4096B, host-endian
  std::vector<uint8_t> psConst;  // b1: 3584B, host-endian
  std::vector<eot::render::DeclElem> decl;
  uint32_t declHash = 0;
  float alphaThreshold = 0.0f;
  bool windowSpace = false;
  struct Slot { uint32_t addr = 0; eot::render::TextureFetch fetch; } slots[16];
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
    case 0x1A2286:
    case 0x1A2386: return RenderFormat::R8G8B8A8_UINT;
    case 0x2C2359: return RenderFormat::R16G16_SINT;
    case 0x1A235A: return RenderFormat::R16G16B16A16_SNORM;
    case 0x1A2086:
    case 0x1A2186: return RenderFormat::R8G8B8A8_UNORM;
    case 0x2C2159: return RenderFormat::R16G16_SNORM;
    case 0x1A215A: return RenderFormat::R16G16B16A16_SNORM;
    case 0x2C2059: return RenderFormat::R16G16_UNORM;
    case 0x1A205A: return RenderFormat::R16G16B16A16_UNORM;
    case 0x2C82A1: return RenderFormat::R32_UINT;
    case 0x2A2287:
    case 0x1A2287: return RenderFormat::R32_UINT;
    case 0x2A2187:
    case 0x2A2190:
    case 0x2A2390:
    case 0x1A2187:
    case 0x1A2190:
    case 0x1A2390: return RenderFormat::R32_UINT;
    case 0x2C235F: return RenderFormat::R16G16_FLOAT;
    case 0x1A2360: return RenderFormat::R16G16B16A16_FLOAT;
    default: return RenderFormat::UNKNOWN;
  }
}

uint32_t HashDecl(const std::vector<eot::render::DeclElem>& d) {
  uint32_t h = 2166136261u;
  for (const auto& e : d) {
    for (uint32_t v : {uint32_t(e.usage), uint32_t(e.usageIndex), e.type, uint32_t(e.offset)}) {
      h = (h ^ v) * 16777619u;
    }
  }
  return h;
}

inline void CaptureBoundTexture(CapturedDraw& d) {
  eot::render::GetBoundTexture(0, d.texAddr, d.texFetch);
}

std::vector<uint8_t> SnapshotConstants(uint8_t* base, uint32_t va, uint32_t bytes) {
  std::vector<uint8_t> out(bytes);
  for (uint32_t i = 0; i < bytes; i += 4) {
    uint32_t word = gmem::ReadU32(base, va + i);
    if ((word & 0x7FFFFFFFu) > 0x7F800000u) word = 0u;
    std::memcpy(&out[i], &word, 4);
  }
  return out;
}

void CaptureRealShaderState(CapturedDraw& d, uint8_t* base, uint32_t deviceVA) {
  using namespace eot::render;
  if (deviceVA < 0x1000) return;
  ResolveShadersForDraw(base, deviceVA);
  d.vs = CurrentVertexShader();
  d.ps = CurrentPixelShader();
  d.windowSpace = CurrentVsIsWindowSpace();
  if (!d.vs || !d.ps) return;
  d.vsConst = SnapshotConstants(base, deviceVA + kVsConstOffset, kVsConstBytes);
  d.psConst = SnapshotConstants(base, deviceVA + kPsConstOffset, kPsConstBytes);
  for (uint32_t s = 0; s < 16; ++s)
    GetBoundTexture(s, d.slots[s].addr, d.slots[s].fetch);
  const uint32_t pDecl = gmem::ReadU32(base, deviceVA + kDeclHandleOffset);
  if (DeclElementsFor(base, pDecl, d.decl)) d.declHash = HashDecl(d.decl);
}

std::vector<CapturedDraw> g_frameDraws;
std::mutex g_drawMutex;

std::atomic<uint32_t> g_dvEnter{0}, g_divEnter{0}, g_bvEnter{0};
std::atomic<uint32_t> g_divNoIB{0}, g_divNoVB{0}, g_divNoIBAddr{0}, g_divOk{0}, g_divOk32{0};
std::atomic<uint32_t> g_divPrim{0xFFFFFFFF};

void PushCapturedDraw(CapturedDraw&& d) {
  std::lock_guard<std::mutex> lock(g_drawMutex);
  if (g_frameDraws.size() < 8192) g_frameDraws.push_back(std::move(d));
}

std::unique_ptr<RenderShader> g_dbgVS, g_dbgPS;
std::unique_ptr<RenderPipelineLayout> g_solidLayout;
std::unordered_map<uint32_t, std::unique_ptr<RenderPipeline>> g_psoByStride;
std::vector<std::unique_ptr<RenderBuffer>> g_frameBuffers;

bool EnsureReplayResources() {
  RenderDevice* dev = eot::render::Device();
  if (!dev) return false;
  if (!g_dbgVS)
    g_dbgVS = dev->createShader(g_debug_vs_dxil, sizeof(g_debug_vs_dxil), "VSMain",
                                RenderShaderFormat::DXIL);
  if (!g_dbgPS)
    g_dbgPS = dev->createShader(g_debug_ps_dxil, sizeof(g_debug_ps_dxil), "PSMain",
                                RenderShaderFormat::DXIL);
  if (!g_solidLayout) {
    RenderPipelineLayoutBuilder lb;
    lb.begin(false, true);
    lb.end();
    g_solidLayout = lb.create(dev);
  }
  return g_dbgVS && g_dbgPS && g_solidLayout;
}

RenderPipeline* GetPSO(uint32_t stride) {
  auto it = g_psoByStride.find(stride);
  if (it != g_psoByStride.end()) return it->second.get();
  RenderDevice* dev = eot::render::Device();
  const RenderInputElement elems[] = {
      RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, 0),
  };
  RenderInputSlot slot(0, stride);
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_solidLayout.get();
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
  RenderDevice* dev = eot::render::Device();
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
  desc.pipelineLayout = eot::render::TexturedPipelineLayout();
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
  RenderDevice* dev = eot::render::Device();
  if (!dev) return nullptr;

  std::vector<RenderInputElement> elems;
  elems.reserve(d.decl.size());
  for (const eot::render::DeclElem& e : d.decl) {
    const char* sem = UsageSemantic(e.usage);
    RenderFormat fmt = DeclTypeFormat(e.type);
    if ((e.usage == 3 || e.usage == 6 || e.usage == 7) && e.type == 0x2A23B9)
      fmt = RenderFormat::R32G32B32_UINT;
    else if (e.usage == 5 && e.type == 0x2C2359)
      fmt = RenderFormat::R16G16_UINT;
    else if (e.usage == 5 && e.type == 0x1A235A)
      fmt = RenderFormat::R16G16B16A16_UINT;
    if (!sem || fmt == RenderFormat::UNKNOWN) {
      static std::atomic<uint32_t> s_pn{0};
      if (s_pn.fetch_add(1, std::memory_order_relaxed) < 24)
        REXGPU_INFO("[psores] unmapped decl elem: usage={} type=0x{:06X} (sem={})", e.usage, e.type,
                    sem ? sem : "?");
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
  desc.pipelineLayout = eot::render::GameLayout();
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
    for (uint32_t i = 1; i + 1 < vc; ++i) idx.insert(idx.end(), {0u, i, i + 1});
  } else if (prim == 6 && vc >= 3) {
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
    uint32_t center = 0, prev = 0; int n = 0;
    for (uint32_t v : in) {
      if (v == 0xFFFF || v == 0xFFFFFFFF) { n = 0; continue; }
      if (n == 0) center = v;
      else if (n == 1) prev = v;
      else { out.push_back(V(center)); out.push_back(V(prev)); out.push_back(V(v)); prev = v; }
      ++n;
    }
  } else if (prim == 6) {
    out.reserve(in.size() * 3);
    uint32_t a = 0, b = 0; int n = 0; bool ccw = false;
    for (uint32_t v : in) {
      if (v == 0xFFFF || v == 0xFFFFFFFF) { n = 0; ccw = false; continue; }
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

namespace eot::render {

void SetStreamSource(uint8_t* base, uint32_t stream, uint32_t vbObject, uint32_t offset,
                     uint32_t stride) {
  if (stream != 0) return;
  const uint32_t base0 = (vbObject >= 0x1000) ? gmem::ReadU32(base, vbObject + 0x18) : 0;
  g_s0Base.store(base0, std::memory_order_relaxed);
  g_s0Offset.store(offset, std::memory_order_relaxed);
  g_s0Stride.store(stride, std::memory_order_relaxed);
}

void SetIndices(uint32_t ibObject) {
  g_ibObject.store(ibObject, std::memory_order_relaxed);
}

void SetBoundTexture(uint32_t sampler, uint32_t addr, const TextureFetch& fetch) {
  if (sampler >= 16) return;
  std::lock_guard<std::mutex> lock(g_boundMutex);
  g_bound[sampler] = BoundTex{addr, fetch};
}

bool GetBoundTexture(uint32_t sampler, uint32_t& addr, TextureFetch& fetch) {
  if (sampler >= 16) return false;
  std::lock_guard<std::mutex> lock(g_boundMutex);
  const BoundTex& b = g_bound[sampler];
  if (!b.addr) return false;
  addr = b.addr;
  fetch = b.fetch;
  return true;
}

void DrawVertices(uint8_t* base, uint32_t device, uint32_t prim, uint32_t startVertex,
                  uint32_t vertexCount) {
  g_dvEnter.fetch_add(1, std::memory_order_relaxed);
  const uint32_t baseAddr = g_s0Base.load(std::memory_order_relaxed) & ~3u;
  const uint32_t off = g_s0Offset.load(std::memory_order_relaxed);
  const uint32_t stride = g_s0Stride.load(std::memory_order_relaxed);
  if (!baseAddr || !stride || vertexCount == 0 || vertexCount > 200000) return;
  const uint32_t startVA = baseAddr + off + startVertex * stride;
  if (startVA < 0x1000) return;
  const uint32_t bytes = vertexCount * stride;

  CapturedDraw d;
  d.stride = stride;
  d.vertexCount = vertexCount;
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
                vertexCount, stride, startVA, x, y);
  }
  PushCapturedDraw(std::move(d));
}

void BeginVertices(uint8_t* base, uint32_t device, uint32_t prim, uint32_t vertexCount,
                   uint32_t stride, uint32_t ringPtr) {
  g_bvEnter.fetch_add(1, std::memory_order_relaxed);
  if (ringPtr < 0x1000 || !stride || vertexCount == 0 || vertexCount > 200000) return;
  CapturedDraw d;
  d.immediateVA = ringPtr;
  d.stride = stride;
  d.vertexCount = vertexCount;
  d.prim = prim;
  CaptureBoundTexture(d);
  CaptureRealShaderState(d, base, device);
  PushCapturedDraw(std::move(d));
}

void DrawIndexedVertices(uint8_t* base, uint32_t device, uint32_t prim, int32_t baseVertexIndex,
                         uint32_t startIndex, uint32_t indexCount) {
  g_divEnter.fetch_add(1, std::memory_order_relaxed);
  if (g_divPrim.load(std::memory_order_relaxed) == 0xFFFFFFFF)
    g_divPrim.store(prim, std::memory_order_relaxed);
  const uint32_t ib = g_ibObject.load(std::memory_order_relaxed);
  const uint32_t vbBase = g_s0Base.load(std::memory_order_relaxed) & ~3u;
  const uint32_t vbOff = g_s0Offset.load(std::memory_order_relaxed);
  const uint32_t stride = g_s0Stride.load(std::memory_order_relaxed);
  if (ib < 0x1000) { g_divNoIB.fetch_add(1, std::memory_order_relaxed); return; }
  if (!vbBase || !stride) { g_divNoVB.fetch_add(1, std::memory_order_relaxed); return; }
  if (indexCount == 0 || indexCount > 1000000) return;

  const uint32_t ibAddr = gmem::ReadU32(base, ib + 0x18);
  if (ibAddr < 0x1000) { g_divNoIBAddr.fetch_add(1, std::memory_order_relaxed); return; }
  g_divOk.fetch_add(1, std::memory_order_relaxed);

  const bool idx32 = (gmem::ReadU32(base, ib) & 0x80000000u) != 0;
  if (idx32) g_divOk32.fetch_add(1, std::memory_order_relaxed);
  std::vector<uint32_t> raw(indexCount);
  uint32_t maxIdx = 0;
  for (uint32_t i = 0; i < indexCount; ++i) {
    const uint32_t v = idx32 ? gmem::ReadU32(base, ibAddr + (startIndex + i) * 4)
                             : gmem::ReadU16(base, ibAddr + (startIndex + i) * 2);
    raw[i] = v;
    const uint32_t restart = idx32 ? 0xFFFFFFFFu : 0xFFFFu;
    if (v != restart && v > maxIdx) maxIdx = v;
  }
  std::vector<uint32_t> tris = ExpandIndexed(prim, raw, baseVertexIndex);
  if (tris.empty()) return;

  const uint32_t vertCount = static_cast<uint32_t>(baseVertexIndex) + maxIdx + 1;
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
  PushCapturedDraw(std::move(d));
}

void ReplayCapturedDraws(RenderCommandList* cmd, uint32_t w, uint32_t h) {
  if (!cmd || !EnsureReplayResources()) return;
  std::vector<CapturedDraw> draws;
  {
    std::lock_guard<std::mutex> lock(g_drawMutex);
    draws.swap(g_frameDraws);
  }
  if (draws.empty()) return;

  RenderDevice* dev = eot::render::Device();
  g_frameBuffers.clear();
  EnsureTextureSystem();
  BeginTextureFrame();

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
  uint32_t cntReal = 0, cntTex = 0, cntSolid = 0, cntSkip = 0, cntSkipNoWvp = 0;
  uint32_t cntNoShader = 0, cntNoDecl = 0, cntPsoNull = 0, cntIndexed = 0, cntWindow = 0;
  for (CapturedDraw& d : draws) {
    std::vector<uint32_t> idx = d.indexed ? std::move(d.indices) : BuildIndices(d.prim, d.vertexCount);
    if (idx.empty() || d.verts.empty()) { ++cntSkip; continue; }
    if (d.indexed) ++cntIndexed;
    if (d.windowSpace) ++cntWindow;
    if (!d.vs || !d.ps) ++cntNoShader;
    else if (d.decl.empty()) ++cntNoDecl;

    RenderPipeline* realPso = (d.vs && d.ps && !d.decl.empty()) ? GetRealPSO(d) : nullptr;
    if (d.vs && d.ps && !d.decl.empty() && !realPso) ++cntPsoNull;

    if (d.indexed && !realPso) { ++cntSkipNoWvp; continue; }

    if (realPso && d.windowSpace) {
      for (const eot::render::DeclElem& e : d.decl) {
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
      texIndex = GetOrCreateTextureIndex(cmd, d.texAddr, d.texFetch);

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
        uint32_t idx2 = GetOrCreateTextureIndex(cmd, d.slots[s].addr, d.slots[s].fetch);
        WriteU32LE(shared, s * 4, idx2);
        WriteU32LE(shared, 256 + s * 4, 0);
      }
      WriteF32LE(shared, 356, 1.0f / static_cast<float>(w));   // g_HalfPixelOffset.x (c22.y)
      WriteF32LE(shared, 360, -1.0f / static_cast<float>(h));  // g_HalfPixelOffset.y
      WriteF32LE(shared, 364, d.alphaThreshold);               // g_AlphaThreshold (c22.w)
      {
        auto bswap = [](uint32_t t) {
          switch (t) {
            case 0x2C2359: case 0x1A235A: case 0x2C2159: case 0x1A215A:
            case 0x2C2059: case 0x1A205A: case 0x2C235F: case 0x1A2360: return true;
            default: return false;
          }
        };
        uint32_t swPos = 0, swTex = 0, swNrm = 0, swTan = 0, swBin = 0, swBw = 0, sintTex = 0;
        for (const eot::render::DeclElem& e : d.decl) {
          const uint32_t bit = 1u << (e.usageIndex & 31);
          const bool bs = bswap(e.type);
          switch (e.usage) {
            case 0: if (bs) swPos |= bit; break;
            case 1: if (bs) swBw  |= bit; break;
            case 3: if (bs) swNrm |= bit; break;
            case 5:
              if (bs) swTex |= bit;
              if (e.type == 0x2C2359 || e.type == 0x1A235A) sintTex |= bit;
              break;
            case 6: if (bs) swTan |= bit; break;
            case 7: if (bs) swBin |= bit; break;
          }
        }
        WriteU32LE(shared, 352, swTex);
        WriteU32LE(shared, 368, swNrm);
        WriteU32LE(shared, 372, swBin);
        WriteU32LE(shared, 376, swTan);
        WriteU32LE(shared, 380, swBw);
        WriteU32LE(shared, 384, swPos);
        WriteU32LE(shared, 388, sintTex);
      }
      std::unique_ptr<RenderBuffer> shCb =
          dev->createBuffer(RenderBufferDesc::UploadBuffer(kSharedBytes, RenderBufferFlag::CONSTANT));
      if (void* p = shCb->map()) { std::memcpy(p, shared, kSharedBytes); shCb->unmap(); }

      uint32_t cbvVS = 0, cbvPS = 0, cbvShared = 0;
      GameCbvIndices(cbvVS, cbvPS, cbvShared);
      cmd->setGraphicsPipelineLayout(GameLayout());
      cmd->setPipeline(realPso);
      cmd->setGraphicsDescriptorSet(TextureSet(), 0);
      cmd->setGraphicsDescriptorSet(Tex3DSet(), 1);
      cmd->setGraphicsDescriptorSet(TexCubeSet(), 2);
      cmd->setGraphicsDescriptorSet(SamplerSet(), 3);
      cmd->setGraphicsDescriptorSet(Tex1DSet(), 4);
      cmd->setGraphicsRootDescriptor(vsCb->at(0), cbvVS);
      cmd->setGraphicsRootDescriptor(psCb->at(0), cbvPS);
      cmd->setGraphicsRootDescriptor(shCb->at(0), cbvShared);
      g_frameBuffers.push_back(std::move(vsCb));
      g_frameBuffers.push_back(std::move(psCb));
      g_frameBuffers.push_back(std::move(shCb));
      ++cntReal;
    } else if (texIndex != 0) {
      cmd->setGraphicsPipelineLayout(TexturedPipelineLayout());
      cmd->setPipeline(GetTexturedPSO(d.stride));
      cmd->setGraphicsDescriptorSet(TextureSet(), 0);
      cmd->setGraphicsDescriptorSet(SamplerSet(), 1);
      cmd->setGraphicsPushConstants(0, &texIndex, 0, sizeof(texIndex));
      ++cntTex;
    } else {
      cmd->setGraphicsPipelineLayout(g_solidLayout.get());
      cmd->setPipeline(GetPSO(d.stride));
      ++cntSolid;
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
  if ((s_log++ % 120) == 0) {
    REXGPU_INFO(
        "[draw] replayed {} ({} captured) | path real={} tex={} solid={} skip={} | "
        "indexed={} window={} | drop noShader={} noDecl={} psoNull={} skipNoWvp={}",
        drawn, draws.size(), cntReal, cntTex, cntSolid, cntSkip, cntIndexed, cntWindow,
        cntNoShader, cntNoDecl, cntPsoNull, cntSkipNoWvp);
    REXGPU_INFO(
        "[draw] ENTRIES DrawVtx={} BeginVtx={} DrawIndexed={} | indexed early-out: "
        "noIB={} noVB={} noIBAddr={} ok={} ok32={} firstPrim={}",
        g_dvEnter.exchange(0), g_bvEnter.exchange(0), g_divEnter.exchange(0),
        g_divNoIB.exchange(0), g_divNoVB.exchange(0), g_divNoIBAddr.exchange(0),
        g_divOk.exchange(0), g_divOk32.exchange(0), g_divPrim.load());
  }
}

static constexpr uint32_t kMovieTexIndex = 8190;
static std::mutex g_movieMutex;
static std::vector<uint8_t> g_movieRGBA;
static uint32_t g_movieW = 0, g_movieH = 0;
static bool g_movieHasFrame = false;
static int g_movieStale = 0;

static std::unique_ptr<RenderTexture> g_movieTex;
static std::unique_ptr<RenderTextureView> g_movieView;
static uint32_t g_movieTexW = 0, g_movieTexH = 0;
static std::unique_ptr<RenderBuffer> g_movieStaging, g_movieQuadVB, g_movieQuadIB;

static inline uint8_t Clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v)); }

void SubmitMovieFrame(uint8_t* base, uint32_t yuvVA) {
  if (!base || yuvVA < 0x1000) return;
  const uint32_t yPtr = gmem::ReadU32(base, yuvVA + 0x00), yPitch = gmem::ReadU32(base, yuvVA + 0x08);
  const uint32_t uPtr = gmem::ReadU32(base, yuvVA + 0x0C), uPitch = gmem::ReadU32(base, yuvVA + 0x14);
  const uint32_t vPtr = gmem::ReadU32(base, yuvVA + 0x18), vPitch = gmem::ReadU32(base, yuvVA + 0x20);
  const uint32_t w = gmem::ReadU32(base, yuvVA + 0x38), h = gmem::ReadU32(base, yuvVA + 0x3C);
  if (w == 0 || h == 0 || w > 4096 || h > 4096) return;
  if (yPtr < 0x1000 || uPtr < 0x1000 || vPtr < 0x1000) return;
  const uint8_t* Y = static_cast<const uint8_t*>(gmem::GuestAddressToHostMutable(yPtr));
  const uint8_t* U = static_cast<const uint8_t*>(gmem::GuestAddressToHostMutable(uPtr));
  const uint8_t* V = static_cast<const uint8_t*>(gmem::GuestAddressToHostMutable(vPtr));
  if (!Y || !U || !V) return;

  std::lock_guard<std::mutex> lk(g_movieMutex);
  g_movieRGBA.resize(size_t(w) * h * 4);
  uint8_t* out = g_movieRGBA.data();
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* yr = Y + size_t(y) * yPitch;
    const uint8_t* ur = U + size_t(y >> 1) * uPitch;
    const uint8_t* vr = V + size_t(y >> 1) * vPitch;
    uint8_t* o = out + size_t(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x, o += 4) {
      const int C = int(yr[x]) - 16, D = int(ur[x >> 1]) - 128, E = int(vr[x >> 1]) - 128;
      o[0] = Clamp8((298 * C + 409 * E + 128) >> 8);
      o[1] = Clamp8((298 * C - 100 * D - 208 * E + 128) >> 8);
      o[2] = Clamp8((298 * C + 516 * D + 128) >> 8);
      o[3] = 255;
    }
  }
  g_movieW = w;
  g_movieH = h;
  g_movieHasFrame = true;
  g_movieStale = 0;
}

bool PresentMovieFrame(RenderCommandList* cmd, uint32_t w, uint32_t h) {
  if (!cmd) return false;
  RenderDevice* dev = eot::render::Device();
  if (!dev || !EnsureTextureSystem()) return false;

  std::vector<uint8_t> rgba;
  uint32_t mw, mh;
  {
    std::lock_guard<std::mutex> lk(g_movieMutex);
    if (!g_movieHasFrame) return false;
    if (++g_movieStale > 6) {
      g_movieHasFrame = false;
      return false;
    }
    if (g_movieRGBA.empty() || g_movieW == 0 || g_movieH == 0) return false;
    rgba = g_movieRGBA;
    mw = g_movieW;
    mh = g_movieH;
  }

  if (!g_movieTex || g_movieTexW != mw || g_movieTexH != mh) {
    g_movieTex = dev->createTexture(RenderTextureDesc::Texture2D(mw, mh, 1, RenderFormat::R8G8B8A8_UNORM));
    g_movieView = g_movieTex->createTextureView(RenderTextureViewDesc::Texture2D(RenderFormat::R8G8B8A8_UNORM));
    g_movieTexW = mw;
    g_movieTexH = mh;
    TextureSet()->setTexture(kMovieTexIndex, g_movieTex.get(), RenderTextureLayout::SHADER_READ,
                             g_movieView.get());
  }

  const uint32_t rowData = mw * 4;
  const uint32_t rowPitch = (rowData + 255) & ~255u;
  g_movieStaging = dev->createBuffer(RenderBufferDesc::UploadBuffer(uint64_t(rowPitch) * mh));
  if (auto* p = static_cast<uint8_t*>(g_movieStaging->map())) {
    for (uint32_t y = 0; y < mh; ++y)
      std::memcpy(p + size_t(y) * rowPitch, rgba.data() + size_t(y) * rowData, rowData);
    g_movieStaging->unmap();
  }
  cmd->barriers(RenderBarrierStage::COPY,
                RenderTextureBarrier(g_movieTex.get(), RenderTextureLayout::COPY_DEST));
  cmd->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(g_movieTex.get(), 0),
      RenderTextureCopyLocation::PlacedFootprint(g_movieStaging.get(), RenderFormat::R8G8B8A8_UNORM,
                                                 mw, mh, 1, rowPitch / 4));
  cmd->barriers(RenderBarrierStage::GRAPHICS,
                RenderTextureBarrier(g_movieTex.get(), RenderTextureLayout::SHADER_READ));

  struct MV {
    float x, y;
    uint32_t bgra;
    float u, v;
  };
  const MV verts[4] = {
      {0.0f, 0.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
      {kGameWidth, 0.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
      {0.0f, kGameHeight, 0xFFFFFFFFu, 0.0f, 1.0f},
      {kGameWidth, kGameHeight, 0xFFFFFFFFu, 1.0f, 1.0f},
  };
  const uint32_t idx[6] = {0, 1, 2, 1, 3, 2};
  g_movieQuadVB = dev->createBuffer(RenderBufferDesc::VertexBuffer(sizeof(verts), RenderHeapType::UPLOAD));
  if (void* p = g_movieQuadVB->map()) { std::memcpy(p, verts, sizeof(verts)); g_movieQuadVB->unmap(); }
  g_movieQuadIB = dev->createBuffer(RenderBufferDesc::IndexBuffer(sizeof(idx), RenderHeapType::UPLOAD));
  if (void* p = g_movieQuadIB->map()) { std::memcpy(p, idx, sizeof(idx)); g_movieQuadIB->unmap(); }

  cmd->setViewports(RenderViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)));
  cmd->setScissors(RenderRect(0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)));
  cmd->setGraphicsPipelineLayout(TexturedPipelineLayout());
  cmd->setPipeline(GetTexturedPSO(sizeof(MV)));
  cmd->setGraphicsDescriptorSet(TextureSet(), 0);
  cmd->setGraphicsDescriptorSet(SamplerSet(), 1);
  uint32_t ti = kMovieTexIndex;
  cmd->setGraphicsPushConstants(0, &ti, 0, sizeof(ti));
  RenderVertexBufferView vbv(g_movieQuadVB.get(), sizeof(verts));
  RenderInputSlot slot(0, sizeof(MV));
  cmd->setVertexBuffers(0, &vbv, 1, &slot);
  RenderIndexBufferView ibv(g_movieQuadIB.get(), sizeof(idx), RenderFormat::R32_UINT);
  cmd->setIndexBuffer(&ibv);
  cmd->drawIndexedInstanced(6, 1, 0, 0, 0);
  return true;
}

}
