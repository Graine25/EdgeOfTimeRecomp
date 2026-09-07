#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/shaders/shader_cache.h"

namespace eot::gpu {

constexpr u32 kInvalidDescriptorIndex = ~u32{0};

enum : u32 {
  kNullTexture2DDescriptorIndex = 0,
  kNullTexture3DDescriptorIndex = 1,
  kNullTextureCubeDescriptorIndex = 2,
  kNullTextureDescriptorCount = 3,
};

constexpr u32 kIdentityFetchSwizzle = 0x688;

struct HostTexture {
  std::unique_ptr<plume::RenderTexture> texture;
  plume::RenderTextureDesc desc;
  std::unique_ptr<plume::RenderTextureView> srv;
  u32 descriptorIndex = kInvalidDescriptorIndex;
  struct SwizzledSrv {
    std::unique_ptr<plume::RenderTextureView> view;
    u32 descriptorIndex = kInvalidDescriptorIndex;
  };
  std::unordered_map<u32, SwizzledSrv> swizzledSrvs;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  plume::RenderFormat viewFormat = plume::RenderFormat::UNKNOWN;
  plume::RenderTextureViewDimension viewDimension =
      plume::RenderTextureViewDimension::TEXTURE_2D;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 mipLevels = 1;
  u32 arraySize = 1;
  u32 sampleCount = 1;
  bool isDepth = false;
  bool renderable = false;
  bool needsClear = false;
  std::vector<std::unique_ptr<plume::RenderTextureView>> mipViews;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> mipFramebuffers;

  bool valid() const { return texture != nullptr; }
};

struct GuestTexture {
  u32 va = 0;
  u32 fetch[6] = {};
  rex::graphics::xenos::TextureFormat format =
      rex::graphics::xenos::TextureFormat::k_8_8_8_8;
  rex::graphics::xenos::DataDimension dimension =
      rex::graphics::xenos::DataDimension::k2DOrStacked;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 mipLevels = 1;
  bool tiled = false;
  u32 baseAddress = 0;
  u32 mipAddress = 0;
  bool gammaSigned = false;

  HostTexture host;

  bool uploaded = false;
  u64 uploadedUnlockSeq = 0;
  bool uploadFailed = false;
  bool resolveOwned = false;
  u32 resolvedMipMask = 0;
  u64 lastUseFrame = 0;
  u64 lastSampledFrame = 0;
  u64 lastResolvedFrame = 0;
};

struct GuestSurface {
  u32 va = 0;
  u32 surfaceInfo = 0; // +0x18
  u32 info = 0;        // +0x1C  RB_COLOR_INFO / RB_DEPTH_INFO word
  u32 hiControl = 0;   // +0x20
  u32 sizeBits = 0;    // +0x24
  u32 formatWord = 0;  // +0x28
  u32 width = 0;
  u32 height = 0;
  u32 msaaSamples = 1;
  bool isDepth = false;
  u32 colorFormat = 0;
  u32 depthFormat = 0;
  u32 baseTile = 0;
  i32 colorExpBias = 0;
  float scale = 1.0f;

  HostTexture host;
  bool drawn = false;
  u64 lastUseFrame = 0;
};

struct VertexInput {
  u8 usage = 0;
  u8 usageIndex = 0;
};

struct GuestShader {
  u32 va = 0;
  u64 hash = 0;
  bool isPixel = false;
  const ShaderCacheEntry *entry = nullptr;
  bool cacheMissLogged = false;
  bool createdByGuestCall = false;
  std::vector<VertexInput> inputs;
  bool usesFloatConstants = true;
  u32 floatConstantRegs = 256;
};

}
