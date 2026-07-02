#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>

#include "src/goliath_engine/gpu/renderer/guest_resources.h"
#include "src/goliath_engine/gpu/renderer/render_internal.h"
#include "src/goliath_engine/kernel/guest_memory.h"

namespace gmem = eot::kernel::memory;
using namespace plume;

namespace {

constexpr uint32_t kMaxTextures = 8192;

namespace xe = rex::graphics;
namespace xenos = rex::graphics::xenos;
namespace txc = rex::graphics::texture_conversion;

uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

struct HostTexture {
  std::unique_ptr<RenderTexture> tex;
  std::unique_ptr<RenderTextureView> view;
  uint32_t index = 0;
  uint64_t hash = 0;
};
std::unordered_map<uint32_t, HostTexture> g_textures;
uint32_t g_nextIndex = 1;
std::vector<std::unique_ptr<RenderBuffer>> g_staging;
std::vector<std::unique_ptr<RenderTexture>> g_retiredTex;
std::vector<std::unique_ptr<RenderTextureView>> g_retiredView;

enum class Decode {
  kNone,
  kCTX1,
  kDXT3A,
  kD24S8,
};

bool MapXenosFormat(xenos::TextureFormat fmt, RenderFormat& host, Decode& decode) {
  decode = Decode::kNone;
  switch (fmt) {
    case xenos::TextureFormat::k_DXT1:    host = RenderFormat::BC1_UNORM; return true;
    case xenos::TextureFormat::k_DXT2_3:  host = RenderFormat::BC2_UNORM; return true;
    case xenos::TextureFormat::k_DXT4_5:  host = RenderFormat::BC3_UNORM; return true;
    case xenos::TextureFormat::k_DXN:     host = RenderFormat::BC5_UNORM; return true;
    case xenos::TextureFormat::k_DXT5A:   host = RenderFormat::BC4_UNORM; return true;
    case xenos::TextureFormat::k_DXT3A:   host = RenderFormat::BC2_UNORM; decode = Decode::kDXT3A; return true;
    case xenos::TextureFormat::k_CTX1:    host = RenderFormat::R8G8_UNORM; decode = Decode::kCTX1; return true;
    case xenos::TextureFormat::k_8_8_8_8: host = RenderFormat::B8G8R8A8_UNORM; return true;
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:     host = RenderFormat::R8_UNORM; return true;
    case xenos::TextureFormat::k_8_8:     host = RenderFormat::R8G8_UNORM; return true;
    case xenos::TextureFormat::k_24_8:    host = RenderFormat::R32_FLOAT; decode = Decode::kD24S8; return true;
    default: return false;
  }
}

std::atomic<uint64_t> g_declCount{0};
std::unordered_map<uint32_t, std::vector<eot::render::DeclElem>> g_declsByHandle;
std::mutex g_declMutex;

const char* UsageName(uint32_t u) {
  switch (u) {
    case 0: return "POSITION"; case 1: return "BLENDWEIGHT"; case 2: return "BLENDINDICES";
    case 3: return "NORMAL"; case 4: return "PSIZE"; case 5: return "TEXCOORD";
    case 6: return "TANGENT"; case 7: return "BINORMAL"; case 8: return "TESSFACTOR";
    case 9: return "POSITIONT"; case 10: return "COLOR"; case 11: return "FOG";
    case 12: return "DEPTH"; case 13: return "SAMPLE"; default: return "?";
  }
}

}

namespace eot::render {

void BeginTextureFrame() {
  g_staging.clear();
  g_retiredTex.clear();
  g_retiredView.clear();
}

uint32_t GetOrCreateTextureIndex(RenderCommandList* cmd, uint32_t guestAddr,
                                 const TextureFetch& fetch) {
  if (!guestAddr || !cmd || !EnsureTextureSystem()) return 0;

  xenos::xe_gpu_texture_fetch_t fc{};
  std::memcpy(&fc, fetch.dword, sizeof(uint32_t) * 6);

  xe::TextureInfo info;
  if (!xe::TextureInfo::Prepare(fc, &info)) return 0;
  if (!info.memory.base_address || !info.memory.base_size) return 0;

  const xenos::TextureFormat baseFmt = xe::GetBaseFormat(info.format);
  RenderFormat hostFormat;
  Decode decode;
  if (!MapXenosFormat(baseFmt, hostFormat, decode)) {
    static std::unordered_map<uint32_t, bool> s_warned;
    const uint32_t k = static_cast<uint32_t>(baseFmt);
    if (!s_warned[k]) { s_warned[k] = true; REXGPU_INFO("texture: unhandled xenos format {}", k); }
    return 0;
  }

  const xe::FormatInfo* srcFi = info.format_info();
  const uint32_t srcBpb = srcFi->bytes_per_block();
  const uint32_t srcBlockW = srcFi->block_width, srcBlockH = srcFi->block_height;
  const uint32_t texelW = info.width + 1, texelH = info.height + 1;
  if (texelW == 0 || texelH == 0 || texelW > 8192 || texelH > 8192) return 0;

  const uint32_t visBlocksW = (texelW + srcBlockW - 1) / srcBlockW;
  const uint32_t visBlocksH = (texelH + srcBlockH - 1) / srcBlockH;
  const uint32_t srcPitchBlocks = info.extent.block_pitch_h;

  const auto* src =
      static_cast<const uint8_t*>(gmem::GuestAddressToHostMutable(info.memory.base_address));
  if (!src) return 0;

  const uint64_t hash = XXH3_64bits(src, info.memory.base_size);
  auto it = g_textures.find(guestAddr);
  if (it != g_textures.end() && it->second.hash == hash) return it->second.index;

  std::vector<uint8_t> srcLinear(size_t(visBlocksW) * visBlocksH * srcBpb);
  const xenos::Endian endian = info.endianness;
  if (info.is_tiled) {
    txc::UntileInfo ui{};
    ui.offset_x = 0;
    ui.offset_y = 0;
    ui.width = visBlocksW;
    ui.height = visBlocksH;
    ui.input_pitch = srcPitchBlocks;
    ui.output_pitch = visBlocksW;
    ui.input_format_info = srcFi;
    ui.output_format_info = srcFi;
    ui.copy_callback = [endian](void* o, const void* i, size_t len) {
      txc::CopySwapBlock(endian, o, i, len);
    };
    txc::Untile(srcLinear.data(), src, &ui);
  } else {
    for (uint32_t by = 0; by < visBlocksH; ++by)
      txc::CopySwapBlock(endian, srcLinear.data() + size_t(by) * visBlocksW * srcBpb,
                         src + size_t(by) * srcPitchBlocks * srcBpb, size_t(visBlocksW) * srcBpb);
  }

  const uint8_t* hostData = srcLinear.data();
  std::vector<uint8_t> decoded;
  uint32_t hostRowUnits = visBlocksW;
  uint32_t hostRows = visBlocksH;
  uint32_t hostBpb = srcBpb;
  uint32_t hostBlockTexels = srcBlockW;
  uint32_t hostSrcRowStride = visBlocksW * srcBpb;

  if (decode == Decode::kDXT3A) {
    hostBpb = 16;
    hostBlockTexels = 4;
    hostSrcRowStride = visBlocksW * hostBpb;
    decoded.resize(size_t(visBlocksW) * visBlocksH * hostBpb);
    for (uint32_t b = 0; b < visBlocksW * visBlocksH; ++b)
      txc::ConvertTexelDXT3AToDXT3(xenos::Endian::kNone, decoded.data() + size_t(b) * 16,
                                   srcLinear.data() + size_t(b) * srcBpb, 16);
    hostData = decoded.data();
  } else if (decode == Decode::kD24S8) {
    hostBpb = 4;
    hostBlockTexels = 1;
    hostSrcRowStride = visBlocksW * hostBpb;
    decoded.resize(size_t(visBlocksW) * visBlocksH * hostBpb);
    for (uint32_t t = 0; t < visBlocksW * visBlocksH; ++t) {
      uint32_t v;
      std::memcpy(&v, srcLinear.data() + size_t(t) * 4, 4);
      const float depth = static_cast<float>(v >> 8) * (1.0f / 16777215.0f);
      std::memcpy(decoded.data() + size_t(t) * 4, &depth, 4);
    }
    hostData = decoded.data();
  } else if (decode == Decode::kCTX1) {
    hostBpb = 2;
    hostBlockTexels = 1;
    const uint32_t padW = visBlocksW * 4, padH = visBlocksH * 4;
    hostRowUnits = padW;
    hostRows = padH;
    hostSrcRowStride = padW * hostBpb;
    decoded.resize(size_t(padW) * padH * hostBpb);
    for (uint32_t by = 0; by < visBlocksH; ++by)
      for (uint32_t bx = 0; bx < visBlocksW; ++bx)
        txc::ConvertTexelCTX1ToR8G8(
            xenos::Endian::kNone,
            decoded.data() + (size_t(by) * 4 * padW + size_t(bx) * 4) * hostBpb,
            srcLinear.data() + (size_t(by) * visBlocksW + bx) * srcBpb, padW * hostBpb);
    hostData = decoded.data();
  }

  const uint32_t rowDataBytes = hostRowUnits * hostBpb;
  const uint32_t rowPitch = AlignUp(rowDataBytes, 256);
  const uint32_t rowWidthTexels = (rowPitch / hostBpb) * hostBlockTexels;
  RenderDevice* dev = Device();
  std::unique_ptr<RenderBuffer> staging =
      dev->createBuffer(RenderBufferDesc::UploadBuffer(uint64_t(rowPitch) * hostRows));
  if (auto* p = static_cast<uint8_t*>(staging->map())) {
    for (uint32_t r = 0; r < hostRows; ++r)
      std::memcpy(p + size_t(r) * rowPitch, hostData + size_t(r) * hostSrcRowStride, rowDataBytes);
    staging->unmap();
  }

  std::unique_ptr<RenderTexture> tex =
      dev->createTexture(RenderTextureDesc::Texture2D(texelW, texelH, 1, hostFormat));
  cmd->barriers(RenderBarrierStage::COPY,
                RenderTextureBarrier(tex.get(), RenderTextureLayout::COPY_DEST));
  cmd->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(tex.get(), 0),
      RenderTextureCopyLocation::PlacedFootprint(staging.get(), hostFormat, texelW, texelH, 1,
                                                 rowWidthTexels));
  cmd->barriers(RenderBarrierStage::GRAPHICS,
                RenderTextureBarrier(tex.get(), RenderTextureLayout::SHADER_READ));

  std::unique_ptr<RenderTextureView> view =
      tex->createTextureView(RenderTextureViewDesc::Texture2D(hostFormat));
  const uint32_t index = (it != g_textures.end()) ? it->second.index : g_nextIndex++;
  TextureSet()->setTexture(index, tex.get(), RenderTextureLayout::SHADER_READ, view.get());
  g_staging.push_back(std::move(staging));
  if (it != g_textures.end()) {
    g_retiredTex.push_back(std::move(it->second.tex));
    g_retiredView.push_back(std::move(it->second.view));
  }
  g_textures[guestAddr] = HostTexture{std::move(tex), std::move(view), index, hash};
  return index;
}

void RegisterVertexDeclaration(uint8_t* base, uint32_t pElems, uint32_t pDecl) {
  std::vector<DeclElem> elems;
  for (uint32_t i = 0; i < 32; ++i) {
    const uint32_t e = pElems + i * 12;  // sizeof(D3DVERTEXELEMENT9) on X360
    const uint32_t d0 = gmem::ReadU32(base, e + 0);  // Stream<<16 | Offset
    const uint16_t stream = static_cast<uint16_t>(d0 >> 16);
    if (stream == 0xFF) break;  // D3DDECL_END
    DeclElem el;
    el.stream = stream;
    el.offset = static_cast<uint16_t>(d0 & 0xFFFF);
    el.type = gmem::ReadU32(base, e + 4);
    const uint32_t muu = gmem::ReadU32(base, e + 8);  // Method|Usage|UsageIndex
    el.usage = static_cast<uint8_t>((muu >> 16) & 0xFF);
    el.usageIndex = static_cast<uint8_t>((muu >> 8) & 0xFF);
    elems.push_back(el);
  }
  if (elems.empty()) return;
  std::lock_guard<std::mutex> lock(g_declMutex);
  g_declsByHandle[pDecl] = std::move(elems);
}

static bool ParseDeclObject(uint8_t* base, uint32_t pDecl, std::vector<DeclElem>& out) {
  if (pDecl < 0x1000) return false;
  if (gmem::ReadU32(base, pDecl + 0) != 0x100005u) return false;
  if (gmem::ReadU32(base, pDecl + 0x14) != 0xFFFF0000u) return false;
  const uint32_t count = gmem::ReadU32(base, pDecl + 24);
  if (count == 0 || count > 32) return false;
  std::vector<DeclElem> elems;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t b = pDecl + 52 + i * 12;
    const uint32_t d0 = gmem::ReadU32(base, b + 0);
    const uint16_t stream = static_cast<uint16_t>(d0 >> 16);
    if (stream == 0xFF) break;
    DeclElem el;
    el.stream = stream;
    el.offset = static_cast<uint16_t>(d0 & 0xFFFF);
    el.type = gmem::ReadU32(base, b + 4);
    const uint32_t muu = gmem::ReadU32(base, b + 8);
    el.usage = static_cast<uint8_t>((muu >> 16) & 0xFF);
    el.usageIndex = static_cast<uint8_t>((muu >> 8) & 0xFF);
    elems.push_back(el);
  }
  if (elems.empty()) return false;
  out = std::move(elems);
  return true;
}

bool DeclElementsFor(uint8_t* base, uint32_t pDecl, std::vector<DeclElem>& out) {
  {
    std::lock_guard<std::mutex> lock(g_declMutex);
    auto it = g_declsByHandle.find(pDecl);
    if (it != g_declsByHandle.end()) {
      out = it->second;
      return true;
    }
  }
  return ParseDeclObject(base, pDecl, out);
}

}

REX_EXTERN(__imp__D3DDevice_CreateVertexShader);
REX_HOOK_RAW(D3DDevice_CreateVertexShader) {
  const uint32_t pFunction = ctx.r3.u32;
  __imp__D3DDevice_CreateVertexShader(ctx, base);
  eot::render::RegisterShader(base, pFunction, ctx.r3.u32, true);
}

REX_EXTERN(__imp__D3DDevice_CreatePixelShader);
REX_HOOK_RAW(D3DDevice_CreatePixelShader) {
  const uint32_t pFunction = ctx.r3.u32;
  __imp__D3DDevice_CreatePixelShader(ctx, base);
  eot::render::RegisterShader(base, pFunction, ctx.r3.u32, false);
}

REX_EXTERN(__imp__sub_82116810);
REX_HOOK_RAW(sub_82116810) {
  const uint32_t stream = ctx.r3.u32;
  const uint32_t pos = stream >= 0x1000
                           ? gmem::ReadU32(base, stream + 24) + gmem::ReadU32(base, stream + 32)
                           : 0;
  __imp__sub_82116810(ctx, base);
  const uint32_t node = ctx.r3.u32;
  if (node >= 0x1000 && pos >= 0x1000) {
    const uint32_t obj = gmem::ReadU32(base, node + 36);
    if (obj >= 0x1000) eot::render::DiagStreamShader(base, pos, obj, true);
  }
}

REX_EXTERN(__imp__sub_82116B78);
REX_HOOK_RAW(sub_82116B78) {
  const uint32_t stream = ctx.r3.u32;
  const uint32_t pos = stream >= 0x1000
                           ? gmem::ReadU32(base, stream + 24) + gmem::ReadU32(base, stream + 32)
                           : 0;
  __imp__sub_82116B78(ctx, base);
  const uint32_t node = ctx.r3.u32;
  if (node >= 0x1000 && pos >= 0x1000) {
    const uint32_t obj = gmem::ReadU32(base, node + 32);  // PS object @ node+32 (VS uses +36)
    if (obj >= 0x1000) eot::render::DiagStreamShader(base, pos, obj, false);
  }
}

REX_EXTERN(__imp__XGSetVertexDeclaration);
REX_HOOK_RAW(XGSetVertexDeclaration) {
  const uint32_t pElems = ctx.r3.u32;
  const uint32_t pDecl = ctx.r4.u32;
  if (pElems >= 0x1000 && pDecl >= 0x1000) eot::render::RegisterVertexDeclaration(base, pElems, pDecl);
  uint64_t n = g_declCount.fetch_add(1, std::memory_order_relaxed);
  if (n < 8 && pElems >= 0x1000) {
    REXGPU_INFO("[vtx] DECL #{} elems=0x{:08X} -> decl=0x{:08X}", n, pElems, pDecl);
    for (uint32_t i = 0; i < 32; ++i) {
      const uint32_t e = pElems + i * 12;  // sizeof(D3DVERTEXELEMENT9)
      const uint32_t d0 = gmem::ReadU32(base, e + 0);  // Stream<<16 | Offset
      const uint16_t stream = static_cast<uint16_t>(d0 >> 16);
      if (stream == 0xFF) break;  // D3DDECL_END
      const uint16_t offset = static_cast<uint16_t>(d0 & 0xFFFF);
      const uint32_t type = gmem::ReadU32(base, e + 4);
      const uint32_t muu = gmem::ReadU32(base, e + 8);   // Method|Usage|UsageIndex
      const uint32_t usage = (muu >> 16) & 0xFF, usageIdx = (muu >> 8) & 0xFF;
      REXGPU_INFO("[vtx]   s{} off={:>3} type=0x{:06X} {}{}", stream, offset, type,
                  UsageName(usage), usageIdx);
    }
  }
  __imp__XGSetVertexDeclaration(ctx, base);
}
