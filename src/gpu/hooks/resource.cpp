/**
 * @file    gpu/hooks/resource.cpp
 * @brief   Guest hooks that create and refcount D3D texture/surface
 *          resources.
 *
 * Narrow-slice port of re:Blue's gpu/hooks/resource.cpp: only
 * CreateTexture/CreateSurface/Release/AddRef/GetType, the five whose
 * addresses are already confirmed in reeot_default_xex.toml. Buffer
 * creation/lock, LockRect, GetSurfaceLevel/GetDesc and the native-mirror
 * fallback path aren't ported yet - see gpu/device/device.h and
 * gpu/guest/resources.h for what's deliberately missing underneath these.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/device/device.h"
#include "gpu/device/host_resource_heap.h"
#include "gpu/guest/d3d.h"
#include "gpu/guest/format.h"

namespace {

eot::gpu::GuestTexture *D3DDevice_CreateSurface_hook(u32 width, u32 height,
                                                      u32 format,
                                                      u32 multi_sample,
                                                      u32) {
  const plume::RenderFormat plume_format =
      eot::gpu::ConvertGuestFormat(format);
  const bool is_depth = eot::gpu::IsDepthFormat(plume_format);

  const plume::RenderSampleCounts msaa_count =
      (multi_sample != 0 && eot::gpu::Video::CvarMSAASampleCount() !=
                                 plume::RenderSampleCount::COUNT_1)
          ? eot::gpu::Video::CvarMSAASampleCount()
          : plume::RenderSampleCount::COUNT_1;

  auto *surface = eot::gpu::HostResourceHeap::Alloc<eot::gpu::GuestTexture>(
      is_depth ? eot::gpu::ResourceType::DepthStencil
               : eot::gpu::ResourceType::RenderTarget);
  if (!surface) {
    EOT_ERROR("CreateSurface: host resource arena exhausted");
    return nullptr;
  }

  eot::gpu::InitResourceHeader(surface->x360.as_surface.resource,
                               eot::gpu::D3DResourceType::kSurface);

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = plume_format;
  desc.flags = is_depth ? plume::RenderTextureFlag::DEPTH_TARGET
                        : plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount = msaa_count;
  desc.committed = true;

  auto *device = eot::gpu::Video::HostDevice();
  if (device) {
    surface->textureHolder =
        eot::gpu::CreateHostTexture(device, desc, "rt-surface");
    surface->texture = surface->textureHolder.get();
  } else {
    EOT_ERROR("CreateSurface fired before Video host device exists");
  }
  surface->width = width;
  surface->height = height;
  surface->format = plume_format;
  surface->guestFormat = format;
  surface->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  surface->sampleCount = desc.multisampling.sampleCount;
  return surface;
}

eot::gpu::GuestTexture *
D3DDevice_CreateTexture_hook(u32 width, u32 height, u32 depth, u32 levels,
                             u32, u32 format, u32,
                             u32 d3d_type) {
  const bool is_volume = (d3d_type == 17);
  const bool is_cube = (d3d_type == 18);
  const auto rtype = is_volume ? eot::gpu::ResourceType::VolumeTexture
                               : eot::gpu::ResourceType::Texture;
  const auto view_dimension =
      is_volume ? plume::RenderTextureViewDimension::TEXTURE_3D
      : is_cube ? plume::RenderTextureViewDimension::TEXTURE_CUBE
                : plume::RenderTextureViewDimension::TEXTURE_2D;
  auto *texture =
      eot::gpu::HostResourceHeap::Alloc<eot::gpu::GuestTexture>(rtype);
  if (!texture) {
    EOT_ERROR("CreateTexture: host resource arena exhausted");
    return nullptr;
  }

  eot::gpu::InitResourceHeader(texture->x360.as_texture.resource,
                               eot::gpu::D3DResourceType::kTexture);

  const plume::RenderFormat plume_format =
      eot::gpu::ConvertGuestFormat(format);
  plume::RenderTextureDesc desc;
  desc.dimension = (rtype == eot::gpu::ResourceType::VolumeTexture)
                       ? plume::RenderTextureDimension::TEXTURE_3D
                       : plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = is_volume ? depth : 1;
  desc.mipLevels = levels;
  desc.arraySize = is_cube ? 6 : 1;
  desc.format = plume_format;
  if (is_cube) {
    desc.flags = plume::RenderTextureFlag::CUBE;
    if (eot::gpu::IsDepthFormat(plume_format)) {
      desc.flags |= plume::RenderTextureFlag::DEPTH_TARGET;
    }
  } else if (eot::gpu::IsDepthFormat(plume_format)) {
    desc.flags = plume::RenderTextureFlag::DEPTH_TARGET;
  } else if (eot::gpu::IsRenderTargetCapable(plume_format)) {
    desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  } else {
    desc.flags = plume::RenderTextureFlag::NONE;
  }
  const bool is_rt_or_ds =
      (desc.flags & (plume::RenderTextureFlag::RENDER_TARGET |
                     plume::RenderTextureFlag::DEPTH_TARGET)) != 0;
  desc.committed = is_rt_or_ds;

  texture->viewDimension = view_dimension;

  auto *device = eot::gpu::Video::HostDevice();
  if (device) {
    texture->textureHolder =
        eot::gpu::CreateHostTexture(device, desc, "guest-texture");
    texture->texture = texture->textureHolder.get();
  } else {
    EOT_ERROR("CreateTexture fired before Video host device exists");
  }
  texture->width = width;
  texture->height = height;
  texture->depth = depth;
  texture->mipLevels = levels;
  texture->format = plume_format;
  texture->guestFormat = format;
  return texture;
}

u32 D3DResource_Release_hook(rex::MappedPtr<eot::gpu::D3DResource> res) {
  if (!res)
    return 0;
  eot::gpu::ResourceType type;
  if (!eot::gpu::HostResourceHeap::GetType(res.guest_address(), &type)) {
    return 0;
  }
  const u32 prev = res->ReferenceCount;
  if (prev == 0)
    return 0;
  const u32 next = prev - 1;
  res->ReferenceCount = next;
  if (next == 0) {
    eot::gpu::Video::QueueResourceDestroy(res.guest_address(), type);
  }
  return next;
}

u32 D3DResource_AddRef_hook(rex::MappedPtr<eot::gpu::D3DResource> res) {
  if (!res)
    return 0;
  eot::gpu::ResourceType ignored;
  if (!eot::gpu::HostResourceHeap::GetType(res.guest_address(), &ignored)) {
    return 0;
  }
  const u32 next = u32(res->ReferenceCount) + 1;
  res->ReferenceCount = next;
  return next;
}

u32 D3DResource_GetType_hook(u32 res_guest) {
  eot::gpu::ResourceType type;
  if (eot::gpu::HostResourceHeap::GetType(res_guest, &type)) {
    switch (type) {
    case eot::gpu::ResourceType::RenderTarget:
    case eot::gpu::ResourceType::DepthStencil:
      return 1; // D3DRTYPE_SURFACE
    case eot::gpu::ResourceType::Texture:
      return 3; // D3DRTYPE_TEXTURE
    case eot::gpu::ResourceType::VolumeTexture:
      return 17; // D3DRTYPE_VOLUMETEXTURE (X360 = 0x11)
    case eot::gpu::ResourceType::VertexBuffer:
      return 6; // D3DRTYPE_VERTEXBUFFER
    case eot::gpu::ResourceType::IndexBuffer:
      return 7; // D3DRTYPE_INDEXBUFFER
    default:
      return 0;
    }
  }
  return 0;
}

}

REX_HOOK(D3DDevice_CreateSurface, D3DDevice_CreateSurface_hook);
REX_HOOK(D3DDevice_CreateTexture, D3DDevice_CreateTexture_hook);
REX_HOOK(D3DResource_Release, D3DResource_Release_hook);
REX_HOOK(D3DResource_AddRef, D3DResource_AddRef_hook);
REX_HOOK(D3DResource_GetType, D3DResource_GetType_hook);
