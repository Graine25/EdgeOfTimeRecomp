#include "gpu/device/native_texture_mirror.h"

#include <atomic>

#include <bit>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "gpu/device/texture_upload.h"
#include "gpu/guest/texture_fetch.h"

namespace tu = rex::graphics::texture_util;
namespace tc = rex::graphics::texture_conversion;
namespace xenos = rex::graphics::xenos;

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
std::atomic<u32> g_native_built{0};
std::atomic<u32> g_native_unmapped_format{0};
std::atomic<u32> g_native_undescribed{0};

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

GuestTexture *BuildNativeLocked(u32 texture_va, const GuestTextureFetch &f,
                                plume::RenderFormat format) {
  auto *device = Video::HostDevice();
  if (!device)
    return nullptr;

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = f.width;
  desc.height = f.height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = format;
  desc.flags = plume::RenderTextureFlag::NONE;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.committed = true;

  auto mirror = std::make_unique<GuestTexture>(ResourceType::Texture);
  mirror->textureHolder = CreateHostTexture(device, desc, "native-texture");
  if (!mirror->textureHolder)
    return nullptr;
  mirror->texture = mirror->textureHolder.get();
  mirror->selfVa = texture_va;
  mirror->width = f.width;
  mirror->height = f.height;
  mirror->mipLevels = 1;
  mirror->format = format;
  mirror->guestFormat = static_cast<u32>(f.format);
  mirror->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  mirror->sampleCount = desc.multisampling.sampleCount;

  auto *raw = mirror.get();
  g_mirrors[texture_va] = std::move(mirror);
  return raw;
}

bool UntileBaseLevelLocked(GuestTexture *tex, const GuestTextureFetch &f) {
  auto *memory = REX_KERNEL_MEMORY();
  const auto *src = memory->TranslateVirtual<const u8 *>(f.baseAddress);
  if (!src)
    return false;

  const bool compressed = IsBlockCompressed(tex->format);
  const u32 unit = compressed ? BytesPerBlock(tex->format)
                              : BytesPerTexel(tex->format);
  if (!unit)
    return false;
  const u32 edge = compressed ? kTextureBlockSize : 1;

  const u32 units_x = (f.width + edge - 1) / edge;
  const u32 units_y = (f.height + edge - 1) / edge;
  const u32 dst_row = (units_x * unit + 255u) & ~255u;

  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, f.pitchTiles, f.width, f.height, 1,
      f.tiled, f.format, false, true,
      0);

  const u32 pitch_units = layout.base.row_pitch_bytes / unit;
  const u32 extent = layout.base.level_data_extent_bytes;
  if (!pitch_units || !extent) {
    static std::atomic<u32> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 3) {
      EOT_WARN("[native] {}x{} fmt={} has no usable layout (pitchTiles={} "
               "row_pitch={} extent={})",
               f.width, f.height, static_cast<u32>(f.format), f.pitchTiles,
               layout.base.row_pitch_bytes, extent);
    }
    return false;
  }
  const u32 unit_log2 = static_cast<u32>(std::countr_zero(unit));

  std::vector<u8> dst(size_t(dst_row) * units_y, 0u);
  for (u32 y = 0; y < units_y; ++y) {
    for (u32 x = 0; x < units_x; ++x) {
      const i32 offset = tu::GetTiledOffset2D(static_cast<i32>(x),
                                              static_cast<i32>(y), pitch_units,
                                              unit_log2);
      if (offset < 0 || u32(offset) + unit > extent)
        continue;
      tc::CopySwapBlock(f.endianness, dst.data() + size_t(y) * dst_row +
                                          size_t(x) * unit,
                        src + offset, unit);
    }
  }

  QueueNativeUpload(tex, std::move(dst), dst_row, units_y);
  return true;
}

}

GuestTexture *FindOrBuildNativeTexture(u32 texture_va) {
  if (!texture_va)
    return nullptr;
  std::lock_guard lock(g_mutex);

  auto it = g_mirrors.find(texture_va);
  if (it != g_mirrors.end())
    return it->second.get();

  GuestTextureFetch fetch;
  if (!DecodeTextureFetchAt(texture_va + kTextureObjectFetchOffset, fetch)) {
    g_native_undescribed.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  const plume::RenderFormat format = HostFormatForTextureFormat(fetch.format);
  if (format == plume::RenderFormat::UNKNOWN) {
    if (g_native_unmapped_format.fetch_add(1, std::memory_order_relaxed) < 4) {
      EOT_INFO("[native] no host format for Xenos format {} ({}x{})",
               static_cast<u32>(fetch.format), fetch.width, fetch.height);
    }
    return nullptr;
  }
  if (!fetch.width || !fetch.height)
    return nullptr;

  GuestTexture *tex = BuildNativeLocked(texture_va, fetch, format);
  if (tex)
    UntileBaseLevelLocked(tex, fetch);
  if (tex && g_native_built.fetch_add(1, std::memory_order_relaxed) == 0) {
    EOT_INFO("[native] first engine texture mirrored: 0x{:08X} {}x{} fmt={} "
             "mips={} tiled={}",
             texture_va, fetch.width, fetch.height,
             static_cast<u32>(fetch.format), fetch.mipLevels, fetch.tiled);
  }
  return tex;
}

void LogNativeTextureStats() {
  EOT_INFO("[native] {} engine textures mirrored, {} with no host format, "
           "{} undescribed",
           g_native_built.load(), g_native_unmapped_format.load(),
           g_native_undescribed.load());
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
