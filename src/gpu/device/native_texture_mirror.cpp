#include "gpu/device/native_texture_mirror.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/device/host_resource_heap.h"
#include "gpu/guest/d3d.h"
#include "gpu/guest/format.h"

namespace eot::gpu {
namespace {

constexpr u32 kPoolSurfaceOffset = 0x72C;
constexpr u32 kPoolSurfaceCount = 25;
static_assert(sizeof(D3DSurface) == 0x30);

std::mutex g_mutex;
std::unordered_map<u32, std::unique_ptr<GuestTexture>> g_mirrors;

std::atomic<u32> g_built{0};
std::atomic<u32> g_refreshed{0};
std::atomic<u32> g_rejected{0};

GuestTexture *BuildLocked(u32 surface_va, u32 width, u32 height,
                          plume::RenderFormat format, u32 guest_format,
                          bool is_depth) {
  auto *device = Video::HostDevice();
  if (!device)
    return nullptr;

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = format;
  desc.flags = is_depth ? plume::RenderTextureFlag::DEPTH_TARGET
                        : plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.committed = true;

  auto mirror = std::make_unique<GuestTexture>(
      is_depth ? ResourceType::DepthStencil : ResourceType::RenderTarget);
  mirror->textureHolder = CreateHostTexture(device, desc, "pool-surface");
  if (!mirror->textureHolder)
    return nullptr;
  mirror->texture = mirror->textureHolder.get();
  mirror->selfVa = surface_va;
  mirror->width = width;
  mirror->height = height;
  mirror->format = format;
  mirror->guestFormat = guest_format;
  mirror->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  mirror->sampleCount = desc.multisampling.sampleCount;

  auto *raw = mirror.get();
  g_mirrors[surface_va] = std::move(mirror);
  return raw;
}

}

GuestTexture *FindOrBuildSurfaceMirror(u32 surface_va) {
  if (!surface_va)
    return nullptr;
  if (HostResourceHeap::FromGuest<GuestTexture>(surface_va))
    return nullptr;

  const auto *header = eot::mem::try_at<const D3DSurface>(surface_va);
  if (!header)
    return nullptr;

  const u32 size_bits = header->SizeBits;
  const u32 width = SurfaceWidthFromSizeBits(size_bits);
  const u32 height = SurfaceHeightFromSizeBits(size_bits);
  const u32 guest_format = header->Format;

  if (width <= 1 && height <= 1)
    return nullptr;

  const plume::RenderFormat format = ConvertGuestFormat(guest_format);
  if (format == plume::RenderFormat::UNKNOWN) {
    g_rejected.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  const bool is_depth = IsDepthFormat(format);

  std::lock_guard lock(g_mutex);
  auto it = g_mirrors.find(surface_va);
  if (it != g_mirrors.end()) {
    GuestTexture *existing = it->second.get();
    if (existing->width == width && existing->height == height &&
        existing->format == format) {
      g_refreshed.fetch_add(1, std::memory_order_relaxed);
      return existing;
    }
    Video::NotifyTextureDestroyed(existing);
    g_mirrors.erase(it);
  }

  GuestTexture *built =
      BuildLocked(surface_va, width, height, format, guest_format, is_depth);
  if (built) {
    const u32 n = g_built.fetch_add(1, std::memory_order_relaxed);
    if (n < 32) {
      EOT_INFO("[rt] pool surface mirror 0x{:08X}: {}x{} fmt={} {}", surface_va,
               width, height, static_cast<u32>(format),
               is_depth ? "depth" : "colour");
    }
  }
  return built;
}

void RegisterSurfacePool(u32 record_va) {
  if (!record_va)
    return;
  for (u32 i = 0; i < kPoolSurfaceCount; ++i) {
    FindOrBuildSurfaceMirror(record_va + kPoolSurfaceOffset +
                             i * sizeof(D3DSurface));
  }
}

void EvictSurfaceMirror(u32 surface_va) {
  if (!surface_va)
    return;
  std::unique_ptr<GuestTexture> dead;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_mirrors.find(surface_va);
    if (it == g_mirrors.end())
      return;
    dead = std::move(it->second);
    g_mirrors.erase(it);
  }
  Video::NotifyTextureDestroyed(dead.get());
}

GuestTexture *ResolveGuestSurface(u32 surface_va) {
  if (!surface_va)
    return nullptr;
  if (auto *owned = HostResourceHeap::FromGuest<GuestTexture>(surface_va))
    return owned;
  std::lock_guard lock(g_mutex);
  auto it = g_mirrors.find(surface_va);
  return it == g_mirrors.end() ? nullptr : it->second.get();
}

}
