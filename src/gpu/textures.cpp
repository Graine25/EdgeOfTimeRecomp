#include "gpu/textures.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "core/profiling.h"
#include "gpu/device.h"

#include "gpu/settings.h"
#include "gpu/format.h"

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;
namespace tc = rex::graphics::texture_conversion;
using rex::graphics::FormatInfo;
using rex::graphics::TextureInfo;

struct UnlockTable {
  std::mutex mutex;
  std::unordered_map<u32, u64> seq;
  u64 global = 0;
};

UnlockTable &unlocks() {
  static UnlockTable t;
  return t;
}

std::unordered_map<u32, TextureInfo> &infos() {
  static std::unordered_map<u32, TextureInfo> m;
  return m;
}

bool ReadFetch(u32 header_va, u32 out[6]) {
  auto *p = mem::at<be_u32>(header_va + obj::kTextureFetch);
  if (!p)
    return false;
  bool any = false;
  for (u32 i = 0; i < 6; ++i) {
    out[i] = p[i];
    any |= out[i] != 0;
  }
  return any;
}

plume::RenderFormat TypelessFor(plume::RenderFormat f) {
  using F = plume::RenderFormat;
  switch (f) {
  case F::BC1_UNORM:
    return F::BC1_TYPELESS;
  case F::BC2_UNORM:
    return F::BC2_TYPELESS;
  case F::BC3_UNORM:
    return F::BC3_TYPELESS;
  default:
    return f;
  }
}

u32 InfoWidth(const TextureInfo &info) { return info.width + 1; }
u32 InfoHeight(const TextureInfo &info) { return info.height + 1; }
u32 InfoDepth(const TextureInfo &info) { return info.depth + 1; }

bool CreateHostImage(VideoState &s, GuestTexture &t, const TextureInfo &info) {
  HostTexture &host = t.host;
  const TextureFormatMapping m = MapTextureFormat(info.format);
  if (!m.supported) {
    if (!t.uploadFailed) {
      t.uploadFailed = true;
      EOT_WARN("[textures] {:#x}: unsupported guest format {} ({}x{}), sampling null", t.va,
               static_cast<u32>(info.format), InfoWidth(info), InfoHeight(info));
    }
    return false;
  }
  const bool depth = info.format == xe::TextureFormat::k_24_8 ||
                     info.format == xe::TextureFormat::k_24_8_FLOAT;

  plume::RenderTextureDesc desc;
  desc.width = InfoWidth(info);
  desc.height = InfoHeight(info);
  if (m.blockCompressed) {
    desc.width = (desc.width + 3) & ~3u;
    desc.height = (desc.height + 3) & ~3u;
  }
  desc.depth = 1;
  desc.arraySize = 1;
  desc.committed = Settings::CommittedTextures();
  switch (info.dimension) {
  case xe::DataDimension::k3D:
    desc.dimension = plume::RenderTextureDimension::TEXTURE_3D;
    desc.depth = InfoDepth(info);
    host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_3D;
    break;
  case xe::DataDimension::kCube:
    desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    desc.arraySize = 6;
    desc.flags = plume::RenderTextureFlag::CUBE;
    host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_CUBE;
    break;
  case xe::DataDimension::k1D:
  case xe::DataDimension::k2DOrStacked:
  default:
    desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    if (info.is_stacked && InfoDepth(info) > 1)
      desc.arraySize = InfoDepth(info);
    break;
  }
  u32 max_levels = 1;
  for (u32 w = desc.width, h = desc.height; (w > 1 || h > 1) && max_levels < 14; ++max_levels) {
    w = std::max(1u, w >> 1);
    h = std::max(1u, h >> 1);
  }
  desc.mipLevels = std::min(info.mip_max_level + 1u, max_levels);

  if (depth) {
    desc.format = plume::RenderFormat::D32_FLOAT_S8_UINT;
    desc.flags = desc.flags | plume::RenderTextureFlag::DEPTH_TARGET;
    host.viewFormat = plume::RenderFormat::UNKNOWN;
    host.isDepth = true;
  } else {
    const bool srgb_capable = m.srgbFormat != plume::RenderFormat::UNKNOWN;
    desc.format = srgb_capable ? TypelessFor(m.format) : m.format;
    host.viewFormat = srgb_capable ? (t.gammaSigned ? m.srgbFormat : m.format)
                                   : plume::RenderFormat::UNKNOWN;
    if (!m.blockCompressed && desc.dimension != plume::RenderTextureDimension::TEXTURE_3D)
      desc.flags = desc.flags | plume::RenderTextureFlag::RENDER_TARGET;
    host.isDepth = false;
  }
  host.format = desc.format;
  host.width = desc.width;
  host.height = desc.height;
  host.depth = desc.depth;
  host.mipLevels = desc.mipLevels;
  host.arraySize = desc.arraySize;
  host.renderable = (desc.flags & plume::RenderTextureFlag::RENDER_TARGET) ||
                    (desc.flags & plume::RenderTextureFlag::DEPTH_TARGET);
  CreateOrRecycleHostTexture(s, host, desc, "guest-texture");
  if (!host.texture) {
    t.uploadFailed = true;
    return false;
  }
  return true;
}

void UploadFromGuest(VideoState &s, GuestTexture &t, const TextureInfo &info) {
  EOT_CPU_ZONE("texture upload");
  HostTexture &host = t.host;
  if (!host.texture || host.isDepth)
    return;
  PerfScope perf_scope(s.perf.upload_ms);
  s.perf.uploads++;
  const TextureFormatMapping m = MapTextureFormat(info.format);
  if (m.convert) {
    if (!t.uploadFailed) {
      t.uploadFailed = true;
      EOT_WARN("[textures] {:#x}: guest format {} needs a texel conversion the uploader "
               "lacks yet; sampling whatever is there",
               t.va, static_cast<u32>(info.format));
    }
    return;
  }
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, t.fetch, sizeof(fetch));
  const FormatInfo *fi = info.format_info();
  const u32 bpb = fi->bytes_per_block();
  const u32 slices = host.arraySize > 1 ? host.arraySize : host.depth;
  const bool is_3d = info.dimension == xe::DataDimension::k3D;
  const bool has_base = info.memory.base_address != 0;
  const u32 width = InfoWidth(info), height = InfoHeight(info), depth = InfoDepth(info);
  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      info.dimension, fetch.pitch, width, height, is_3d ? depth : slices, info.is_tiled,
      info.format, info.has_packed_mips, has_base, info.mip_max_level);

  TransitionLocked(s, host, plume::RenderTextureLayout::COPY_DEST);
  for (u32 level = info.mip_min_level; level <= info.mip_max_level && level < host.mipLevels;
       ++level) {
    u32 address = 0;
    const tu::TextureGuestLayout::Level *lvl = nullptr;
    u32 x_blocks = 0, y_blocks = 0, z_blocks = 0;
    const bool packed = layout.packed_level != UINT32_MAX && level >= layout.packed_level;
    if (level == 0 && has_base) {
      address = info.memory.base_address;
      lvl = &layout.base;
    } else if (!info.memory.mip_address && layout.packed_level == 0 && has_base) {
      address = info.memory.base_address;
      lvl = &layout.base;
    } else {
      if (!info.memory.mip_address)
        continue;
      const u32 stored = packed ? layout.packed_level : level;
      address = info.memory.mip_address + layout.mip_offsets_bytes[stored];
      lvl = &layout.mips[stored];
    }
    if (packed) {
      tu::GetPackedMipOffset(width, height, is_3d ? depth : 1, info.format, level, x_blocks,
                             y_blocks, z_blocks);
    }
    u32 w = 0, h = 0;
    info.GetMipSize(level, &w, &h);
    if (!address || !w || !h || !lvl->row_pitch_bytes)
      continue;
    if (m.blockCompressed) {
      w = std::max(4u, (w + 3) & ~3u);
      h = std::max(4u, (h + 3) & ~3u);
    }
    if (w > std::max(1u, host.width >> level) || h > std::max(1u, host.height >> level)) {
      w = std::max(1u, host.width >> level);
      h = std::max(1u, host.height >> level);
    }
    const u8 *src_base = mem::at<u8>(address);
    if (!src_base)
      continue;
    const u32 bw = (w + fi->block_width - 1) / fi->block_width;
    const u32 bh = (h + fi->block_height - 1) / fi->block_height;
    const u32 guest_pitch_blocks = lvl->row_pitch_bytes / bpb;
    const u64 host_pitch = (u64(bw) * bpb + kTextureRowPitchAlignment - 1) /
                           kTextureRowPitchAlignment * kTextureRowPitchAlignment;
    const u64 host_slice_bytes = (host_pitch * bh + kTexturePlacementAlignment - 1) /
                                 kTexturePlacementAlignment * kTexturePlacementAlignment;
    const u64 guest_slice_bytes =
        is_3d ? u64(lvl->z_slice_stride_block_rows) * lvl->row_pitch_bytes
              : lvl->array_slice_stride_bytes;
    UploadAlloc staging;
    if (!UploadAllocate(host_slice_bytes * slices, kTexturePlacementAlignment, &staging))
      return;
    for (u32 slice = 0; slice < slices; ++slice) {
      u8 *dst = staging.cpu + slice * host_slice_bytes;
      const u8 *src = src_base + slice * guest_slice_bytes +
                      (is_3d ? u64(z_blocks) * lvl->row_pitch_bytes * lvl->z_slice_stride_block_rows
                             : 0);
      if (info.is_tiled && is_3d) {
        u32 bpb_log2 = 0;
        while ((1u << bpb_log2) < bpb)
          ++bpb_log2;
        for (u32 y = 0; y < bh; ++y) {
          u8 *dst_row = dst + y * host_pitch;
          for (u32 x = 0; x < bw; ++x) {
            const i32 off = tu::GetTiledOffset3D(
                static_cast<i32>(x + x_blocks), static_cast<i32>(y + y_blocks),
                static_cast<i32>(slice + z_blocks), guest_pitch_blocks,
                lvl->z_slice_stride_block_rows, bpb_log2);
            tc::CopySwapBlock(info.endianness, dst_row + x * bpb, src_base + off, bpb);
          }
        }
      } else if (info.is_tiled) {
        tc::UntileInfo ui{};
        ui.offset_x = x_blocks;
        ui.offset_y = y_blocks;
        ui.width = bw;
        ui.height = bh;
        ui.input_pitch = guest_pitch_blocks;
        ui.output_pitch = static_cast<u32>(host_pitch / bpb);
        ui.input_format_info = fi;
        ui.output_format_info = fi;
        const xe::Endian endian = info.endianness;
        ui.copy_callback = [endian](void *o, const void *i, size_t n) {
          tc::CopySwapBlock(endian, o, i, n);
        };
        tc::Untile(dst, src, &ui);
      } else {
        const u8 *row = src + u64(y_blocks) * lvl->row_pitch_bytes + u64(x_blocks) * bpb;
        for (u32 y = 0; y < bh; ++y) {
          tc::CopySwapBlock(info.endianness, dst + y * host_pitch, row + y * lvl->row_pitch_bytes,
                            u64(bw) * bpb);
        }
      }
      const u32 row_width_texels = static_cast<u32>(host_pitch / bpb) * fi->block_width;
      s.command_list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(host.texture.get(), level,
                                                        host.arraySize > 1 ? slice : 0),
          plume::RenderTextureCopyLocation::PlacedFootprint(
              staging.buffer, m.format, w, h, 1, row_width_texels,
              staging.offset + slice * host_slice_bytes),
          0, 0, is_3d ? slice : 0);
    }
  }
  t.uploaded = true;
  if (info.mip_min_level == 0 && info.mip_max_level + 1 >= host.mipLevels)
    host.needsClear = false;
}

}

constexpr u64 kTextureIdleFrames = 120;

void EvictStaleGuestTextures(VideoState &s) {
  EOT_CPU_ZONE("evict guest textures");
  bool evicted = false;
  for (auto it = s.textures.begin(); it != s.textures.end();) {
    const std::shared_ptr<GuestTexture> &slot = it->second;
    if (!slot) {
      infos().erase(it->first);
      it = s.textures.erase(it);
      s.perf.textures_evicted++;
      evicted = true;
      continue;
    }
    if (slot->lastUseFrame + kTextureIdleFrames >= s.guest_frames) {
      ++it;
      continue;
    }
    if (slot.use_count() == 1)
      ParkHostTexture(s, slot->host);
    infos().erase(it->first);
    it = s.textures.erase(it);
    s.perf.textures_evicted++;
    evicted = true;
  }
  if (evicted)
    s.texture_generation.fetch_add(1, std::memory_order_relaxed);
}

void NotifyResourceUnlocked(u32 resource_va) {
  auto &u = unlocks();
  std::lock_guard lock(u.mutex);
  u.seq[resource_va] = ++u.global;
  state().texture_generation.fetch_add(1, std::memory_order_relaxed);
}

u64 ResourceUnlockSeq(u32 resource_va) {
  auto &u = unlocks();
  std::lock_guard lock(u.mutex);
  auto it = u.seq.find(resource_va);
  return it == u.seq.end() ? 0 : it->second;
}

GuestTexture *GetGuestTexture(VideoState &s, u32 header_va, bool create_host_image) {
  if (!header_va)
    return nullptr;
  u32 fetch[6];
  if (!ReadFetch(header_va, fetch))
    return nullptr;
  auto &slot = s.textures[header_va];
  auto same_storage = [](const u32 *a, const u32 *b) {
    auto fmt = [](u32 d1) {
      const u32 f = d1 & 0x3F;
      return (d1 & ~0x3Fu) | (f == 50 ? 6u : f);
    };
    return (a[0] & ~0x7FFFCu) == (b[0] & ~0x7FFFCu) && fmt(a[1]) == fmt(b[1]) &&
           a[2] == b[2] && a[5] == b[5];
  };
  if (slot && same_storage(slot->fetch, fetch)) {
    if (std::memcmp(slot->fetch, fetch, sizeof(fetch)) != 0)
      std::memcpy(slot->fetch, fetch, sizeof(fetch));
    if (create_host_image && !slot->host.texture) {
      const auto info = infos().find(header_va);
      if (info != infos().end() && CreateHostImage(s, *slot, info->second))
        s.texture_generation.fetch_add(1, std::memory_order_relaxed);
    }
    return slot.get();
  }

  xe::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, fetch, sizeof(f));
  TextureInfo info{};
  if (!TextureInfo::Prepare(f, &info)) {
    u32 n;
    if (DiagShouldLog(0x5E00 ^ header_va, &n))
      EOT_WARN("[textures] {:#x}: TextureInfo::Prepare rejected the fetch constant", header_va);
    return nullptr;
  }
  if (slot) {
    u32 n;
    if (DiagShouldLog(0x5EA0 ^ header_va, &n))
      EOT_INFO("[textures] {:#x}: header changed ({}x{} fmt {} base {:#x} -> {}x{} fmt {} base "
               "{:#x}), recreating (x{})",
               header_va, slot->width, slot->height, static_cast<u32>(slot->format),
               slot->baseAddress, InfoWidth(info), InfoHeight(info),
               static_cast<u32>(info.format), info.memory.base_address, n + 1);
    if (slot.use_count() == 1)
      ParkHostTexture(s, slot->host);
    slot.reset();
  }
  for (auto &[other_va, other] : s.textures) {
    if (!other || other_va == header_va || !(other->resolveOwned || other->host.isDepth) ||
        !same_storage(other->fetch, fetch))
      continue;
    u32 n;
    if (DiagShouldLog(0x5EB0 ^ header_va, &n))
      EOT_INFO("[textures] {:#x}: aliases {:#x} ({}x{} fmt {} base {:#x})", header_va, other_va,
               other->width, other->height, static_cast<u32>(other->format), other->baseAddress);
    infos()[header_va] = info;
    slot = other;
    s.texture_generation.fetch_add(1, std::memory_order_relaxed);
    return slot.get();
  }
  s.texture_generation.fetch_add(1, std::memory_order_relaxed);
  auto t = std::make_shared<GuestTexture>();
  t->va = header_va;
  std::memcpy(t->fetch, fetch, sizeof(fetch));
  t->format = info.format;
  t->dimension = info.dimension;
  t->width = InfoWidth(info);
  t->height = InfoHeight(info);
  t->depth = InfoDepth(info);
  t->mipLevels = info.mip_levels();
  t->tiled = info.is_tiled;
  t->baseAddress = info.memory.base_address;
  t->mipAddress = info.memory.mip_address;
  t->gammaSigned = f.sign_x == xe::TextureSign::kGamma;
  infos()[header_va] = info;
  if (create_host_image)
    CreateHostImage(s, *t, info);
  EOT_DEBUG("[textures] {:#x}: {}x{}x{} mips {}..{} fmt {} {} {} base {:#x} mip {:#x} -> host fmt {}",
            header_va, InfoWidth(info), InfoHeight(info), InfoDepth(info), info.mip_min_level,
            info.mip_max_level, static_cast<u32>(info.format), info.is_tiled ? "tiled" : "linear",
            t->gammaSigned ? "gamma" : "linear-sign", t->baseAddress, t->mipAddress,
            static_cast<u32>(t->host.format));
  slot = std::move(t);
  return slot.get();
}

u32 PrepareTextureForSampling(VideoState &s, GuestTexture &t, u32 swizzle) {
  if (!t.host.texture)
    return kInvalidDescriptorIndex;
  const u64 seq = ResourceUnlockSeq(t.va);
  const bool stale = !t.uploaded || seq != t.uploadedUnlockSeq;
  if (stale && t.resolveOwned) {
    u32 n;
    if (seq > t.uploadedUnlockSeq && DiagShouldLog(0x5E80 ^ t.va, &n))
      EOT_INFO("[textures] {:#x}: Unlock on a resolve-owned mirror ignored (seq {} -> {})", t.va,
               t.uploadedUnlockSeq, seq);
    t.uploadedUnlockSeq = seq;
  } else if (stale) {
    auto it = infos().find(t.va);
    if (it != infos().end()) {
      UploadFromGuest(s, t, it->second);
      t.uploadedUnlockSeq = seq;
    }
  }
  t.lastUseFrame = s.guest_frames;
  TransitionLocked(s, t.host, plume::RenderTextureLayout::SHADER_READ);
  return BindTextureSRVSwizzledLocked(s, t.host, swizzle);
}

bool EnsureResolveMirror(VideoState &s, GuestTexture &t, bool depth_source, float scale) {
  const float k = scale > 0.0f ? scale : RenderScaleFactor();
  const auto info_it = infos().find(t.va);
  if (info_it == infos().end())
    return false;
  const TextureInfo &info = info_it->second;

  const TextureFormatMapping mapping = MapTextureFormat(t.format);
  const bool texture_is_depth = t.format == xe::TextureFormat::k_24_8 ||
                                t.format == xe::TextureFormat::k_24_8_FLOAT;
  if (!t.host.texture &&
      (t.dimension != xe::DataDimension::k2DOrStacked ||
       (info.is_stacked && InfoDepth(info) > 1) || !mapping.supported ||
       mapping.blockCompressed || depth_source != texture_is_depth)) {
    if (!CreateHostImage(s, t, info))
      return false;
    s.texture_generation.fetch_add(1, std::memory_order_relaxed);
  }
  if (!t.resolveOwned &&
      (!t.host.texture || (depth_source == t.host.isDepth && t.host.renderable &&
                           t.host.viewFormat == plume::RenderFormat::UNKNOWN))) {
    const plume::RenderFormat want = ResolveDestinationFormat(t.format, depth_source);
    if (want == plume::RenderFormat::UNKNOWN)
      return false;
    const u32 want_w = ScaleDimBy(InfoWidth(info), k), want_h = ScaleDimBy(InfoHeight(info), k);
    if (k != RenderScaleFactor() && (t.host.width != want_w || t.host.height != want_h))
      EOT_INFO("[textures] {:#x}: resolve mirror at x{:.2f}: {}x{}", t.va, k, want_w, want_h);
    if (!t.host.texture || t.host.format != want || t.host.width != want_w ||
        t.host.height != want_h) {
      EOT_DEBUG("[textures] {:#x}: switching mirror to resolve format {} {}x{} (was {} {}x{})",
                t.va, static_cast<u32>(want), want_w, want_h, static_cast<u32>(t.host.format),
                t.host.width, t.host.height);
      const bool replacing_host = t.host.texture != nullptr;
      ParkHostTexture(s, t.host);
      s.texture_generation.fetch_add(1, std::memory_order_relaxed);
      const bool is_depth = depth_source;
      t.host = HostTexture{};
      plume::RenderTextureDesc desc;
      desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
      desc.width = want_w;
      desc.height = want_h;
      desc.depth = 1;
      desc.arraySize = 1;
      u32 max_levels = 1;
      for (u32 w = desc.width, h = desc.height; (w > 1 || h > 1) && max_levels < 14;
           ++max_levels) {
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
      }
      desc.mipLevels = is_depth && replacing_host
                           ? 1u
                           : std::min(info.mip_max_level + 1u, max_levels);
      desc.format = want;
      desc.flags = is_depth ? plume::RenderTextureFlag::DEPTH_TARGET
                            : plume::RenderTextureFlag::RENDER_TARGET;
      desc.committed = Settings::CommittedTextures();
      t.host.format = want;
      t.host.viewFormat = plume::RenderFormat::UNKNOWN;
      t.host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
      t.host.width = desc.width;
      t.host.height = desc.height;
      t.host.depth = 1;
      t.host.mipLevels = desc.mipLevels;
      t.host.arraySize = 1;
      t.host.isDepth = is_depth;
      CreateOrRecycleHostTexture(s, t.host, desc, "resolve-mirror");
      t.host.renderable = t.host.texture != nullptr;
      t.uploaded = false;
      if (!t.host.texture)
        return false;
    }
  }
  if (!t.host.renderable) {
    u32 n;
    if (DiagShouldLog(0x5F00 ^ t.va, &n))
      EOT_WARN("[textures] {:#x}: resolve into a non-renderable mirror (fmt {})", t.va,
               static_cast<u32>(t.host.format));
    return false;
  }
  if (depth_source != t.host.isDepth) {
    u32 n;
    if (DiagShouldLog(0x5F80 ^ t.va, &n))
      EOT_WARN("[textures] {:#x}: {} resolve into a {} mirror", t.va,
               depth_source ? "depth" : "colour", t.host.isDepth ? "depth" : "colour");
    return false;
  }
  return true;
}

plume::RenderFramebuffer *GetMipFramebuffer(VideoState &s, GuestTexture &t, u32 mip) {
  HostTexture &host = t.host;
  if (!host.texture || mip >= host.mipLevels)
    return nullptr;
  if (host.mipFramebuffers.size() < host.mipLevels) {
    host.mipFramebuffers.resize(host.mipLevels);
    host.mipViews.resize(host.mipLevels);
  }
  if (host.mipFramebuffers[mip])
    return host.mipFramebuffers[mip].get();
  plume::RenderTextureViewDesc vd;
  vd.format = host.isDepth ? host.format
              : host.viewFormat != plume::RenderFormat::UNKNOWN ? host.viewFormat
                                                                  : host.format;
  vd.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  vd.mipSlice = mip;
  vd.mipLevels = 1;
  vd.arrayIndex = 0;
  vd.arraySize = 1;
  s.perf.host_views++;
  host.mipViews[mip] = host.texture->createTextureView(vd);
  if (!host.mipViews[mip])
    return nullptr;
  plume::RenderFramebufferDesc fd;
  const plume::RenderTexture *color[1] = {host.texture.get()};
  const plume::RenderTextureView *views[1] = {host.mipViews[mip].get()};
  if (host.isDepth) {
    fd.depthAttachment = host.texture.get();
    fd.depthAttachmentView = host.mipViews[mip].get();
  } else {
    fd.colorAttachments = color;
    fd.colorAttachmentViews = views;
    fd.colorAttachmentsCount = 1;
  }
  host.mipFramebuffers[mip] = s.device->createFramebuffer(fd);
  return host.mipFramebuffers[mip].get();
}

}
