/**
 * @file    gpu/hooks/resource.cpp
 * @brief   Guest hooks that create and refcount D3D texture/surface
 *          resources.
 *
 * Narrow-slice port of re:Blue's gpu/hooks/resource.cpp, corrected against
 * this binary's own IDA database rather than assumed from re:Blue's: only
 * CreateTexture/CreateSurface/Release/AddRef/GetType/GetSurfaceLevel/
 * D3D_DestroyResource are here. Buffer creation/lock, LockRect, GetDesc and
 * the native-mirror fallback path aren't ported yet - see gpu/device/device.h
 * and gpu/guest/resources.h for what's deliberately missing underneath these.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <cstring>
#include <mutex>
#include <unordered_set>

#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/device/host_heap_arena.h"
#include "gpu/device/host_resource_heap.h"
#include "gpu/device/texture_upload.h"
#include "gpu/device/native_texture_mirror.h"
#include "gpu/guest/d3d.h"
#include "gpu/guest/format.h"

namespace {

std::mutex g_surface_level_mutex;
std::unordered_set<u32> g_surface_levels;

void RegisterSurfaceLevel(u32 va) {
  std::lock_guard<std::mutex> lk(g_surface_level_mutex);
  g_surface_levels.insert(va);
}
bool IsSurfaceLevel(u32 va) {
  std::lock_guard<std::mutex> lk(g_surface_level_mutex);
  return g_surface_levels.count(va) != 0;
}
bool TakeSurfaceLevel(u32 va) {
  std::lock_guard<std::mutex> lk(g_surface_level_mutex);
  return g_surface_levels.erase(va) != 0;
}

u32 ReleaseSurfaceLevel(rex::MappedPtr<eot::gpu::D3DResource> res) {
  const u32 surface_va = res.guest_address();
  if (!IsSurfaceLevel(surface_va))
    return 0;
  const u32 prev = res->ReferenceCount;
  if (prev == 0)
    return 0;
  const u32 next = prev - 1;
  res->ReferenceCount = next;
  if (next == 0) {
    TakeSurfaceLevel(surface_va);
    eot::gpu::HostHeapArena::Get().FreeGuest(surface_va);
  }
  return next;
}

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
  if (plume_format == plume::RenderFormat::UNKNOWN) {
  } else if (device) {
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

  const plume::RenderFormat plume_format =
      eot::gpu::ConvertGuestFormat(format);

  auto *texture =
      eot::gpu::HostResourceHeap::Alloc<eot::gpu::GuestTexture>(rtype);
  if (!texture) {
    EOT_ERROR("CreateTexture: host resource arena exhausted");
    return nullptr;
  }

  eot::gpu::InitResourceHeader(texture->x360.as_texture.resource,
                               eot::gpu::D3DResourceType::kTexture);

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
  if (plume_format == plume::RenderFormat::UNKNOWN) {
  } else if (device) {
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
    return ReleaseSurfaceLevel(res);
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
    if (!IsSurfaceLevel(res.guest_address()))
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
  if (IsSurfaceLevel(res_guest))
    return 1; // D3DRTYPE_SURFACE
  return 0;
}

u32 eot_D3DTexture_LockRect_hook(u32 texture_guest, u32 level,
                                 u32, u32,
                                 rex::MappedPtr<be_u32> pBits_out,
                                 rex::MappedPtr<be_u32> pitch_out,
                                 rex::MappedPtr<be_u32> block_size_out,
                                 rex::MappedPtr<be_u32> block_offset_out) {
  if (pBits_out)
    *pBits_out = 0u;
  if (pitch_out)
    *pitch_out = 0u;
  if (block_size_out)
    *block_size_out = 0u;
  if (block_offset_out)
    *block_offset_out = 0u;

  auto *tex =
      eot::gpu::HostResourceHeap::FromGuest<eot::gpu::GuestTexture>(
          texture_guest);
  if (!tex)
    return 0;
  const eot::gpu::TextureFootprint fp =
      eot::gpu::ComputeTextureFootprint(tex, level);
  const eot::gpu::TextureFootprint base = eot::gpu::ComputeTextureFootprint(tex);
  if (!fp.valid() || !base.valid())
    return 0;
  const u32 pitch = fp.pitch;

  if (!tex->mappedMemory) {
    auto *memory = REX_KERNEL_MEMORY();
    tex->mappedMemory =
        memory->SystemHeapAlloc(static_cast<u32>(base.size()), 0x10);
    if (!tex->mappedMemory) {
      EOT_ERROR("LockRect: scratch alloc failed ({} bytes)", base.size());
      return 0;
    }
  }
  if (pBits_out)
    *pBits_out = tex->mappedMemory;
  if (pitch_out)
    *pitch_out = pitch;
  if (level == 0)
    eot::gpu::QueueTextureUpload(tex);
  else
    eot::gpu::NoteMipLockSkipped();
  return tex->mappedMemory;
}

u32 D3DTexture_GetSurfaceLevel_hook(u32 texture_guest, u32 level) {
  eot::gpu::ResourceType parent_type;
  if (!eot::gpu::HostResourceHeap::GetType(texture_guest, &parent_type)) {
    EOT_ERROR("GetSurfaceLevel: texture 0x{:08X} is not one of ours",
              texture_guest);
    return 0;
  }
  const u32 surface_guest = eot::gpu::HostHeapArena::Get().AllocGuest(0x30, 0x10);
  if (!surface_guest)
    return 0;
  auto *surf = eot::mem::at<eot::gpu::D3DSurface>(surface_guest);
  if (!surf)
    return 0;
  std::memset(surf, 0, sizeof(*surf));
  RegisterSurfaceLevel(surface_guest);
  eot::gpu::InitResourceHeader(surf->resource, eot::gpu::D3DResourceType::kSurface);
  surf->SurfaceInfo = texture_guest;
  surf->DepthInfo = level << 28;
  if (auto *parent = eot::mem::at<eot::gpu::D3DResource>(texture_guest)) {
    parent->ReferenceCount = u32(parent->ReferenceCount) + 1;
  }
  return surface_guest;
}

void D3D_DestroyResource_hook(rex::MappedPtr<eot::gpu::D3DResource> res) {
  if (!res)
    return;
  eot::gpu::ResourceType type;
  if (eot::gpu::HostResourceHeap::GetType(res.guest_address(), &type)) {
    eot::gpu::Video::QueueResourceDestroy(res.guest_address(), type);
    return;
  }
  if (TakeSurfaceLevel(res.guest_address())) {
    eot::gpu::HostHeapArena::Get().FreeGuest(res.guest_address());
  }
}

}

REX_HOOK(D3DDevice_CreateSurface, D3DDevice_CreateSurface_hook);
REX_HOOK(D3DDevice_CreateTexture, D3DDevice_CreateTexture_hook);
REX_HOOK(D3DResource_Release, D3DResource_Release_hook);
REX_HOOK(D3DResource_AddRef, D3DResource_AddRef_hook);
REX_HOOK(D3DResource_GetType, D3DResource_GetType_hook);
REX_HOOK(D3DTexture_GetSurfaceLevel, D3DTexture_GetSurfaceLevel_hook);
REX_HOOK(eot_D3DTexture_LockRect, eot_D3DTexture_LockRect_hook);

REX_EXTERN(__imp__eot_RenderTargetResource_PopulateHeaders);
REX_HOOK_RAW(eot_RenderTargetResource_PopulateHeaders) {
  const u32 record = ctx.r3.u32;
  __imp__eot_RenderTargetResource_PopulateHeaders(ctx, base);
  eot::gpu::RegisterSurfacePool(record);
}
REX_HOOK(D3D_DestroyResource, D3D_DestroyResource_hook);
