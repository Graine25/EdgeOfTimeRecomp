/**
 * @file    gpu/guest/resources.h
 * @brief   Host-side record for a guest D3D9 texture/surface.
 *
 * Narrow-slice port: only GuestTexture, the piece the resource-creation hooks
 * (CreateTexture/CreateSurface/Release/AddRef/GetType) need. re:Blue's version
 * also has GuestBuffer, GuestShader and GuestVertexDeclaration for vertex/
 * index buffers and the shader pipeline - not ported yet, add them alongside
 * the hooks that need them.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <memory>
#include <rex/types.h>
#include <unordered_map>
#include <unordered_set>

#include <plume_render_interface.h>

#include "gpu/guest/d3d.h"
#include "gpu/shaders/shader_cache.h"

namespace eot::gpu {

enum class ResourceType : u32 {
  Texture = 0,
  VolumeTexture = 1,
  VertexBuffer = 2,
  IndexBuffer = 3,
  RenderTarget = 4,
  DepthStencil = 5,
  VertexDeclaration = 6,
  VertexShader = 7,
  PixelShader = 8,
};

union GuestTextureX360 {
  D3DTexture as_texture;
  D3DSurface as_surface;
  u8 raw[52];
};
static_assert(sizeof(GuestTextureX360) == 52);

struct GuestTexture {
  GuestTextureX360 x360;

  ResourceType type = ResourceType::Texture;
  u32 selfVa = 0;

  std::unique_ptr<plume::RenderTexture> textureHolder;
  plume::RenderTexture *texture = nullptr;
  std::unique_ptr<plume::RenderTextureView> textureView;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 0;
  u32 mipLevels = 1;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  u32 guestFormat = 0;
  u32 descriptorIndex = ~u32{0};
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  plume::RenderTextureViewDimension viewDimension =
      plume::RenderTextureViewDimension::UNKNOWN;
  plume::RenderSampleCounts sampleCount = plume::RenderSampleCount::COUNT_1;

  u32 mappedMemory = 0;

  std::unordered_map<const plume::RenderTexture *,
                     std::unique_ptr<plume::RenderFramebuffer>>
      framebuffers;
  std::unique_ptr<plume::RenderTextureView> attachmentView;

  GuestTexture() = default;
  explicit GuestTexture(ResourceType t) : type(t) {}
  GuestTexture(const GuestTexture &) = delete;
  GuestTexture &operator=(const GuestTexture &) = delete;
};

struct GuestShader {
  ResourceType type = ResourceType::VertexShader;
  u32 selfVa = 0;

  u32 objectVa = 0;
  u64 hash = 0;

  const ShaderCacheEntry *shaderCacheEntry = nullptr;

  std::unordered_map<u32, std::unique_ptr<plume::RenderShader>> variants;

  GuestShader() = default;
  explicit GuestShader(ResourceType t) : type(t) {}
  GuestShader(const GuestShader &) = delete;
  GuestShader &operator=(const GuestShader &) = delete;
};

constexpr u32 kInvalidDescriptorIndex = ~u32{0};

u32 ComputeTexturePitch(const GuestTexture *tex);

}
