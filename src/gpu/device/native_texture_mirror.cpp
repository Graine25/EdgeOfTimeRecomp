#include "gpu/device/native_texture_mirror.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <algorithm>
#include <list>
#include <map>
#include <tuple>

#include <bit>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/math.h>

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
std::atomic<u32> g_evicted{0};
std::atomic<u32> g_depth_binds{0};
std::atomic<u32> g_resolved_binds{0};
std::atomic<u32> g_page_binds{0};

std::unordered_map<u32, GuestTexture *> g_surface_by_base;

constexpr u32 kGuestPageShift = 12;
std::unordered_map<u32, GuestTexture *> g_mirror_by_page;

std::unordered_map<u32, GuestTexture *> g_resolved_by_physical;

void RegisterByPageLocked(u32 base_address, GuestTexture *tex) {
  if (base_address && tex)
    g_mirror_by_page[base_address >> kGuestPageShift] = tex;
}

constexpr size_t kMaxNativeMirrors = 2048;

std::list<u32> g_lru;
std::unordered_map<u32, std::list<u32>::iterator> g_lru_pos;

std::vector<std::unique_ptr<GuestTexture>> g_evict_graveyard[kNumFrames];

std::mutex g_unmapped_mutex;
std::map<std::tuple<u32, u32, u32>, u32> g_unmapped;

void NoteUnmappedFormat(u32 format, u32 w, u32 h) {
  std::lock_guard lock(g_unmapped_mutex);
  if (g_unmapped.size() < 512)
    ++g_unmapped[{format, w, h}];
}

bool IsDepthTextureFormat(rex::graphics::xenos::TextureFormat f) {
  return f == rex::graphics::xenos::TextureFormat::k_24_8 ||
         f == rex::graphics::xenos::TextureFormat::k_24_8_FLOAT;
}

void TouchLocked(u32 va) {
  auto it = g_lru_pos.find(va);
  if (it != g_lru_pos.end())
    g_lru.erase(it->second);
  g_lru.push_back(va);
  g_lru_pos[va] = std::prev(g_lru.end());
}

void EvictLocked(size_t keep) {
  while (g_mirrors.size() > keep && !g_lru.empty()) {
    const u32 va = g_lru.front();
    g_lru.pop_front();
    g_lru_pos.erase(va);
    auto it = g_mirrors.find(va);
    if (it == g_mirrors.end())
      continue;
    std::unique_ptr<GuestTexture> dead = std::move(it->second);
    g_mirrors.erase(it);
    Video::ReleaseTextureDescriptor(dead->descriptorIndex);
    dead->descriptorIndex = kInvalidDescriptorIndex;
    g_evict_graveyard[Video::CurrentFrameSlot()].push_back(std::move(dead));
    g_evicted.fetch_add(1, std::memory_order_relaxed);
  }
}

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
  mirror->desc_flags = desc.flags;

  auto *raw = mirror.get();
  g_mirrors[surface_va] = std::move(mirror);

  GuestTextureFetch self;
  if (DecodeTextureFetchAt(surface_va + kTextureObjectFetchOffset, self) &&
      self.baseAddress) {
    g_surface_by_base[self.baseAddress] = raw;
    RegisterByPageLocked(self.baseAddress, raw);
  }
  return raw;
}

u32 ClampedMipMaxLevel(const GuestTextureFetch &f) {
  if (!f.mipAddress || f.mipLevels <= 1)
    return 0;
  const u32 raw_max = f.mipLevels - 1;
  const u32 size_max = rex::log2_floor(std::max(f.width, f.height));
  return std::min(raw_max, size_max);
}

GuestTexture *BuildNativeLocked(u32 texture_va, const GuestTextureFetch &f,
                                plume::RenderFormat format, u32 mip_levels) {
  auto *device = Video::HostDevice();
  if (!device)
    return nullptr;

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = f.width;
  desc.height = f.height;
  desc.depth = 1;
  desc.mipLevels = mip_levels;
  desc.arraySize = 1;
  desc.format = format;
  desc.flags = IsBlockCompressed(format)
                   ? plume::RenderTextureFlag::NONE
                   : plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.committed = true;

  auto mirror = std::make_unique<GuestTexture>(ResourceType::Texture);
  mirror->textureHolder = CreateHostTexture(device, desc, "native-texture");
  if (!mirror->textureHolder)
    return nullptr;
  mirror->texture = mirror->textureHolder.get();
  mirror->desc_flags = desc.flags;
  mirror->selfVa = texture_va;
  mirror->width = f.width;
  mirror->height = f.height;
  mirror->mipLevels = mip_levels;
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
  const auto *src = f.physicalAddress
                        ? memory->TranslatePhysical<const u8 *>(f.baseAddress)
                        : memory->TranslateVirtual<const u8 *>(f.baseAddress);
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

  const bool tail_ok =
      f.physicalAddress
          ? memory->TranslatePhysical<const u8 *>(f.baseAddress + extent - 1) != nullptr
          : mem::try_at<const u8>(f.baseAddress + extent - 1) != nullptr;
  if (!extent || !tail_ok) {
    static std::atomic<u32> unreadable{0};
    if (unreadable.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_WARN("[native] physical base 0x{:08X} + {} bytes unusable; skipping "
               "untile",
               f.baseAddress, extent);
    }
    return false;
  }
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

void UntileMipLevelsLocked(GuestTexture *tex, const GuestTextureFetch &f,
                           u32 mip_max_level) {
  if (!mip_max_level || !f.mipAddress)
    return;
  auto *memory = REX_KERNEL_MEMORY();
  const auto *mip_src =
      f.physicalAddress ? memory->TranslatePhysical<const u8 *>(f.mipAddress)
                        : memory->TranslateVirtual<const u8 *>(f.mipAddress);
  if (!mip_src)
    return;

  const bool compressed = IsBlockCompressed(tex->format);
  const u32 unit = compressed ? BytesPerBlock(tex->format)
                              : BytesPerTexel(tex->format);
  if (!unit)
    return;
  const u32 edge = compressed ? kTextureBlockSize : 1;
  const u32 unit_log2 = static_cast<u32>(std::countr_zero(unit));

  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      xenos::DataDimension::k2DOrStacked, f.pitchTiles, f.width, f.height, 1,
      f.tiled, f.format, f.packedMips, true, mip_max_level);

  if (layout.packed_level == 0)
    return;

  const bool tail_ok =
      f.physicalAddress
          ? memory->TranslatePhysical<const u8 *>(
                f.mipAddress + layout.mips_total_extent_bytes - 1) != nullptr
          : mem::try_at<const u8>(f.mipAddress +
                                  layout.mips_total_extent_bytes - 1) !=
                nullptr;
  if (!layout.mips_total_extent_bytes || !tail_ok) {
    static std::atomic<u32> unreadable{0};
    if (unreadable.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_WARN("[native] mip base 0x{:08X} + {} bytes unusable; skipping mip "
               "chain",
               f.mipAddress, layout.mips_total_extent_bytes);
    }
    return;
  }

  static std::atomic<u32> mip_built{0};
  u32 levels_done = 0;
  for (u32 lvl = 1; lvl <= mip_max_level; ++lvl) {
    const u32 lw = std::max(f.width >> lvl, 1u);
    const u32 lh = std::max(f.height >> lvl, 1u);
    const u32 lwb = (lw + edge - 1) / edge;
    const u32 lhb = (lh + edge - 1) / edge;
    const u32 dst_row = (lwb * unit + 255u) & ~255u;

    const bool packed =
        layout.packed_level != UINT32_MAX && lvl >= layout.packed_level;
    const u32 storage_level = packed ? layout.packed_level : lvl;
    const tu::TextureGuestLayout::Level &sl = layout.mips[storage_level];
    if (!sl.row_pitch_bytes)
      continue;
    const u32 pitch_blocks = sl.row_pitch_bytes / unit;
    const u8 *level_src = mip_src + layout.mip_offsets_bytes[storage_level];

    u32 px = 0, py = 0, pz = 0;
    if (packed) {
      tu::GetPackedMipOffset(f.width, f.height, 1, f.format, lvl, px, py, pz);
    }

    std::vector<u8> dst(size_t(dst_row) * lhb, 0u);
    for (u32 by = 0; by < lhb; ++by) {
      for (u32 bx = 0; bx < lwb; ++bx) {
        const i32 offset = tu::GetTiledOffset2D(
            static_cast<i32>(px + bx), static_cast<i32>(py + by),
            pitch_blocks, unit_log2);
        if (offset < 0 || u32(offset) + unit > sl.level_data_extent_bytes)
          continue;
        tc::CopySwapBlock(f.endianness,
                          dst.data() + size_t(by) * dst_row + size_t(bx) * unit,
                          level_src + offset, unit);
      }
    }
    QueueNativeUpload(tex, std::move(dst), dst_row, lhb, lvl);
    ++levels_done;
  }

  if (levels_done && mip_built.fetch_add(1, std::memory_order_relaxed) == 0) {
    EOT_INFO("[native] first mip chain uploaded: {}x{} fmt={} {} levels "
             "(packed_level={})",
             f.width, f.height, static_cast<u32>(f.format), levels_done,
             layout.packed_level);
  }
}

}

GuestTexture *FindOrBuildNativeTextureFromFetch(
    const GuestTextureFetch &fetch, plume::RenderFormat preferred_format) {
  if (!fetch.baseAddress || !fetch.width || !fetch.height)
    return nullptr;
  std::lock_guard lock(g_mutex);

  if (fetch.physicalAddress) {
    if (auto rit = g_resolved_by_physical.find(fetch.baseAddress);
        rit != g_resolved_by_physical.end() && rit->second &&
        rit->second->texture && rit->second->hasContent &&
        rit->second != Video::BoundColorTexture() &&
        rit->second->width == fetch.width &&
        rit->second->height == fetch.height) {
      if (g_resolved_binds.fetch_add(1, std::memory_order_relaxed) == 0) {
        EOT_INFO("[native] fetch 0x{:08X} ({}x{}) served by a resolved surface",
                 fetch.baseAddress, fetch.width, fetch.height);
      }
      return rit->second;
    }
  }

  if (auto sit = g_surface_by_base.find(fetch.baseAddress);
      sit != g_surface_by_base.end() && sit->second) {
    if (g_depth_binds.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_INFO("[native] fetch 0x{:08X} ({}x{} fmt={}) resolved to an existing "
               "surface mirror",
               fetch.baseAddress, fetch.width, fetch.height,
               static_cast<u32>(fetch.format));
    }
    return sit->second;
  }

  auto it = g_mirrors.find(fetch.baseAddress);
  if (it != g_mirrors.end()) {
    TouchLocked(fetch.baseAddress);
    return it->second.get();
  }

  if (!IsDepthTextureFormat(fetch.format)) {
    if (auto pit = g_mirror_by_page.find(fetch.baseAddress >> kGuestPageShift);
        pit != g_mirror_by_page.end() && pit->second) {
      GuestTexture *shared = pit->second;
      if (shared->texture && shared->hasContent &&
          shared->type != ResourceType::DepthStencil &&
          shared->width == fetch.width && shared->height == fetch.height) {
        if (g_page_binds.fetch_add(1, std::memory_order_relaxed) == 0) {
          EOT_INFO("[native] fetch 0x{:08X} ({}x{}) served by the resolved "
                   "surface sharing its page",
                   fetch.baseAddress, fetch.width, fetch.height);
        }
        return shared;
      }
    }
  }

  if (IsDepthTextureFormat(fetch.format)) {
    GuestTexture *depth = Video::BoundDepthTexture();
    if (!depth || !depth->texture || depth->width != fetch.width ||
        depth->height != fetch.height) {
      depth = nullptr;
      for (auto &[va, mirror] : g_mirrors) {
        if (mirror && mirror->texture &&
            mirror->type == ResourceType::DepthStencil &&
            mirror->width == fetch.width && mirror->height == fetch.height) {
          depth = mirror.get();
          break;
        }
      }
    }
    if (depth && depth->texture) {
      if (g_depth_binds.fetch_add(1, std::memory_order_relaxed) == 0) {
        EOT_INFO("[native] depth fetch {}x{} fmt={} bound to the depth "
                 "attachment",
                 fetch.width, fetch.height, static_cast<u32>(fetch.format));
      }
      return depth;
    }
  }

  const plume::RenderFormat format =
      preferred_format != plume::RenderFormat::UNKNOWN
          ? preferred_format
          : HostFormatForTextureFormat(fetch.format);
  if (format == plume::RenderFormat::UNKNOWN) {
    g_native_unmapped_format.fetch_add(1, std::memory_order_relaxed);
    NoteUnmappedFormat(static_cast<u32>(fetch.format), fetch.width,
                       fetch.height);
    return nullptr;
  }

  const u32 mip_max_level = ClampedMipMaxLevel(fetch);
  GuestTexture *tex =
      BuildNativeLocked(fetch.baseAddress, fetch, format, mip_max_level + 1);
  if (tex) {
    UntileBaseLevelLocked(tex, fetch);
    if (mip_max_level)
      UntileMipLevelsLocked(tex, fetch, mip_max_level);
    if (g_native_built.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_INFO("[native] first engine texture mirrored from a fetch constant: "
               "base=0x{:08X} {}x{} fmt={} mips={} tiled={}",
               fetch.baseAddress, fetch.width, fetch.height,
               static_cast<u32>(fetch.format), fetch.mipLevels, fetch.tiled);
    }
  }
  return tex;
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

  const u32 mip_max_level = ClampedMipMaxLevel(fetch);
  GuestTexture *tex =
      BuildNativeLocked(texture_va, fetch, format, mip_max_level + 1);
  if (tex) {
    UntileBaseLevelLocked(tex, fetch);
    if (mip_max_level)
      UntileMipLevelsLocked(tex, fetch, mip_max_level);
  }
  if (tex && g_native_built.fetch_add(1, std::memory_order_relaxed) == 0) {
    EOT_INFO("[native] first engine texture mirrored: 0x{:08X} {}x{} fmt={} "
             "mips={} tiled={}",
             texture_va, fetch.width, fetch.height,
             static_cast<u32>(fetch.format), fetch.mipLevels, fetch.tiled);
  }
  return tex;
}

void DrainEvictedNativeTextures(u32 slot) {
  if (slot >= kNumFrames)
    return;
  std::vector<std::unique_ptr<GuestTexture>> batch;
  {
    std::lock_guard lock(g_mutex);
    batch.swap(g_evict_graveyard[slot]);
  }
  for (auto &tex : batch)
    Video::NotifyTextureDestroyed(tex.get());
}

void LogNativeTextureStats() {
  EOT_INFO("[native] {} engine textures mirrored, {} with no host format, "
           "{} undescribed",
           g_native_built.load(), g_native_unmapped_format.load(),
           g_native_undescribed.load());
  EOT_INFO("[native] {} live mirrors, {} evicted, {} fetches served by an "
           "existing surface",
           g_mirrors.size(), g_evicted.load(), g_depth_binds.load());
  {
    std::lock_guard lock(g_unmapped_mutex);
    std::vector<std::pair<std::tuple<u32, u32, u32>, u32>> ranked(
        g_unmapped.begin(), g_unmapped.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    for (size_t i = 0; i < ranked.size() && i < 6; ++i) {
      const auto &[fmt, w, h] = ranked[i].first;
      EOT_INFO("[native]   unmapped fmt={} {}x{} - {} rejects", fmt, w, h,
               ranked[i].second);
    }
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
  {
    std::lock_guard lock(g_mutex);
    for (auto it = g_surface_by_base.begin(); it != g_surface_by_base.end();) {
      auto mit = g_mirrors.find(surface_va);
      it = (mit != g_mirrors.end() && it->second == mit->second.get())
               ? g_surface_by_base.erase(it)
               : std::next(it);
    }
  }
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

void PublishResolvedSurface(u32 base_address, GuestTexture *tex) {
  if (!base_address || !tex || !tex->texture)
    return;
  std::lock_guard lock(g_mutex);
  g_resolved_by_physical[PhysicalTextureKey(base_address)] = tex;
}

GuestTexture *ResolveMirrorByAddress(u32 address) {
  if (!address)
    return nullptr;
  std::lock_guard lock(g_mutex);
  auto exact = g_mirrors.find(address);
  if (exact != g_mirrors.end())
    return exact->second.get();
  auto page = g_mirror_by_page.find(address >> kGuestPageShift);
  return page == g_mirror_by_page.end() ? nullptr : page->second;
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
