#include "texture.h"

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

#include "src/goliath_engine/gpu/renderer/video.h"
#include "src/goliath_engine/kernel/guest_memory.h"

namespace gmem = eot::kernel::memory;
using namespace plume;

namespace {

constexpr uint32_t kMaxTextures = 8192;

uint32_t XGAddress2DTiledOffset(uint32_t x, uint32_t y, uint32_t Width, uint32_t TexelPitch) {
  const int v4 = 31 - std::countl_zero(TexelPitch);
  const uint32_t v5 = (((4 * y) & 0x18) + (x & 7)) << v4;
  const uint32_t v6 = ((((Width + 31) >> 5) * (y >> 5) + (x >> 5)) << (v4 + 7)) +
                      2 * (((8 * y) & 8) + (v5 & 0xFFFFFFF0)) + ((y & 8) << (v4 + 3)) + (v5 & 0xF);
  return (4 * (2 * (((16 * y) & 0x100) + (v6 & 0xFFFFFE00)) + (v6 & 0x1C0)) +
          ((8 * (((2 * y) & 0xFFFFFFF0) + x)) & 0xC0) + (v6 & 0x3F)) >>
         v4;
}
void UntileBlocks(const uint8_t* src, uint8_t* dst, uint32_t wBlocks, uint32_t hBlocks,
                  uint32_t bpb) {
  for (uint32_t by = 0; by < hBlocks; ++by)
    for (uint32_t bx = 0; bx < wBlocks; ++bx)
      std::memcpy(dst + (by * wBlocks + bx) * bpb,
                  src + XGAddress2DTiledOffset(bx, by, wBlocks, bpb) * bpb, bpb);
}
uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

std::unique_ptr<RenderDescriptorSet> g_texSet;
std::unique_ptr<RenderDescriptorSet> g_tex3DSet;
std::unique_ptr<RenderDescriptorSet> g_texCubeSet;
std::unique_ptr<RenderDescriptorSet> g_tex1DSet;
std::unique_ptr<RenderDescriptorSet> g_sampSet;
std::unique_ptr<RenderSampler> g_sampler;
std::unique_ptr<RenderPipelineLayout> g_layout;
std::unique_ptr<RenderPipelineLayout> g_gameLayout;
uint32_t g_cbvVS = 0, g_cbvPS = 0, g_cbvShared = 0;
bool g_ready = false;

struct GuestTexture {
  std::unique_ptr<RenderTexture> tex;
  std::unique_ptr<RenderTextureView> view;
  uint32_t index = 0;
};
std::unordered_map<uint32_t, GuestTexture> g_textures;
uint32_t g_nextIndex = 1;
std::vector<std::unique_ptr<RenderBuffer>> g_staging;

struct BoundTex {
  uint32_t addr = 0, d0 = 0, d1 = 0, d2 = 0;
};
BoundTex g_bound[16];
std::mutex g_boundMutex;

bool FormatToBC(uint32_t fmt, RenderFormat& rf, uint32_t& bpb) {
  switch (fmt) {
    case 0x12: rf = RenderFormat::BC1_UNORM; bpb = 8; return true;
    case 0x13: rf = RenderFormat::BC2_UNORM; bpb = 16; return true;
    case 0x14: rf = RenderFormat::BC3_UNORM; bpb = 16; return true;
    default: return false;
  }
}

}

namespace eot::gpu {

bool EnsureTextureSystem() {
  if (g_ready) return true;
  RenderDevice* dev = Device();
  if (!dev) return false;

  RenderDescriptorSetBuilder texB;
  texB.begin();
  texB.addTexture(0, kMaxTextures);
  texB.end(true, kMaxTextures);
  g_texSet = texB.create(dev);

  RenderDescriptorSetBuilder sampB;
  sampB.begin();
  sampB.addSampler(0, 1);
  sampB.end();
  g_sampSet = sampB.create(dev);
  RenderSamplerDesc sd;
  sd.minFilter = RenderFilter::LINEAR;
  sd.magFilter = RenderFilter::LINEAR;
  sd.addressU = RenderTextureAddressMode::WRAP;
  sd.addressV = RenderTextureAddressMode::WRAP;
  sd.addressW = RenderTextureAddressMode::WRAP;
  g_sampler = dev->createSampler(sd);
  g_sampSet->setSampler(0, g_sampler.get());

  RenderPipelineLayoutBuilder lb;
  lb.begin(false, true);
  lb.addDescriptorSet(texB);
  lb.addDescriptorSet(sampB);
  lb.addPushConstant(0, 2, 4, RenderShaderStageFlag::PIXEL);
  lb.end();
  g_layout = lb.create(dev);

  RenderDescriptorSetBuilder t3dB, tcB, t1dB;
  t3dB.begin(); t3dB.addTexture(0, 256); t3dB.end(true, 256); g_tex3DSet = t3dB.create(dev);
  tcB.begin();  tcB.addTexture(0, 256);  tcB.end(true, 256);  g_texCubeSet = tcB.create(dev);
  t1dB.begin(); t1dB.addTexture(0, 256); t1dB.end(true, 256); g_tex1DSet = t1dB.create(dev);
  RenderPipelineLayoutBuilder gb;
  gb.begin(false, true);
  gb.addDescriptorSet(texB);
  gb.addDescriptorSet(t3dB);
  gb.addDescriptorSet(tcB);
  gb.addDescriptorSet(sampB);
  gb.addDescriptorSet(t1dB);
  g_cbvVS = gb.addRootDescriptor(0, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
  g_cbvPS = gb.addRootDescriptor(1, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
  g_cbvShared = gb.addRootDescriptor(2, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
  gb.addPushConstant(3, 4, 4, RenderShaderStageFlag::PIXEL);
  gb.end();
  g_gameLayout = gb.create(dev);

  g_ready = g_texSet && g_sampSet && g_layout && g_gameLayout;
  REXGPU_INFO("texture system ready: debugLayout={} gameLayout={} (cbv VS={} PS={} shared={})",
              g_layout != nullptr, g_gameLayout != nullptr, g_cbvVS, g_cbvPS, g_cbvShared);
  return g_ready;
}

void BeginTextureFrame() { g_staging.clear(); }

uint32_t GetOrCreateTextureIndex(RenderCommandList* cmd, uint32_t guestAddr, uint32_t d0,
                                 uint32_t d1, uint32_t d2) {
  if (!guestAddr || !cmd || !EnsureTextureSystem()) return 0;
  auto it = g_textures.find(guestAddr);
  if (it != g_textures.end()) return it->second.index;

  RenderFormat rf;
  uint32_t bpb;
  if (!FormatToBC(d1 & 0x3F, rf, bpb)) return 0;
  const uint32_t w = (d2 & 0x1FFF) + 1, h = ((d2 >> 13) & 0x1FFF) + 1;
  const uint32_t base = d1 & 0xFFFFF000;
  const bool tiled = (d0 >> 31) & 1;
  const auto* src = static_cast<const uint8_t*>(gmem::GuestAddressToHostMutable(base));
  if (!src || w < 4 || h < 4 || w > 4096 || h > 4096) return 0;

  const uint32_t wBlocks = (w + 3) / 4, hBlocks = (h + 3) / 4;
  std::vector<uint8_t> linear(size_t(wBlocks) * hBlocks * bpb);
  if (tiled)
    UntileBlocks(src, linear.data(), wBlocks, hBlocks, bpb);
  else
    std::memcpy(linear.data(), src, linear.size());

  const uint32_t rowPitch = AlignUp(wBlocks * bpb, 256);
  const uint32_t rowWidthTexels = (rowPitch / bpb) * 4;
  RenderDevice* dev = Device();
  std::unique_ptr<RenderBuffer> staging =
      dev->createBuffer(RenderBufferDesc::UploadBuffer(uint64_t(rowPitch) * hBlocks));
  if (auto* p = static_cast<uint8_t*>(staging->map())) {
    for (uint32_t by = 0; by < hBlocks; ++by)
      std::memcpy(p + size_t(by) * rowPitch, linear.data() + size_t(by) * wBlocks * bpb,
                  wBlocks * bpb);
    staging->unmap();
  }

  std::unique_ptr<RenderTexture> tex = dev->createTexture(RenderTextureDesc::Texture2D(w, h, 1, rf));
  cmd->barriers(RenderBarrierStage::COPY,
                RenderTextureBarrier(tex.get(), RenderTextureLayout::COPY_DEST));
  cmd->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(tex.get(), 0),
      RenderTextureCopyLocation::PlacedFootprint(staging.get(), rf, w, h, 1, rowWidthTexels));
  cmd->barriers(RenderBarrierStage::GRAPHICS,
                RenderTextureBarrier(tex.get(), RenderTextureLayout::SHADER_READ));

  std::unique_ptr<RenderTextureView> view = tex->createTextureView(RenderTextureViewDesc::Texture2D(rf));
  const uint32_t index = g_nextIndex++;
  g_texSet->setTexture(index, tex.get(), RenderTextureLayout::SHADER_READ, view.get());
  g_staging.push_back(std::move(staging));
  g_textures[guestAddr] = GuestTexture{std::move(tex), std::move(view), index};
  return index;
}

RenderDescriptorSet* TextureSet() { return g_texSet.get(); }
RenderDescriptorSet* SamplerSet() { return g_sampSet.get(); }
RenderPipelineLayout* TexturedPipelineLayout() { return g_layout.get(); }

RenderPipelineLayout* GameLayout() { return g_gameLayout.get(); }
RenderDescriptorSet* Tex3DSet() { return g_tex3DSet.get(); }
RenderDescriptorSet* TexCubeSet() { return g_texCubeSet.get(); }
RenderDescriptorSet* Tex1DSet() { return g_tex1DSet.get(); }
void GameCbvIndices(uint32_t& vs, uint32_t& ps, uint32_t& shared) {
  vs = g_cbvVS;
  ps = g_cbvPS;
  shared = g_cbvShared;
}

bool GetBoundTexture(uint32_t sampler, uint32_t& addr, uint32_t& d0, uint32_t& d1, uint32_t& d2) {
  if (sampler >= 16) return false;
  std::lock_guard<std::mutex> lock(g_boundMutex);
  const BoundTex& b = g_bound[sampler];
  if (!b.addr) return false;
  addr = b.addr;
  d0 = b.d0;
  d1 = b.d1;
  d2 = b.d2;
  return true;
}

}

REX_EXTERN(__imp__D3DDevice_SetTexture);
REX_HOOK_RAW(D3DDevice_SetTexture) {
  const uint32_t sampler = ctx.r4.u32;
  const uint32_t tex = ctx.r5.u32;
  if (sampler < 16) {
    BoundTex b{};
    if (tex >= 0x1000) {
      b.addr = tex;
      b.d0 = gmem::ReadU32(base, tex + 0x1C + 0);
      b.d1 = gmem::ReadU32(base, tex + 0x1C + 4);
      b.d2 = gmem::ReadU32(base, tex + 0x1C + 8);
    }
    std::lock_guard<std::mutex> lock(g_boundMutex);
    g_bound[sampler] = b;
  }
  __imp__D3DDevice_SetTexture(ctx, base);
}
