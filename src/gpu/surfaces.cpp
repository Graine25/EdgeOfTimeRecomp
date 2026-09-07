#include <algorithm>
#include <format>
#include "gpu/surfaces.h"

#include <memory>

#include <rex/graphics/xenos.h>
#include <rex/memory/utils.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/settings.h"

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;

bool IsDepthFormatWord(u32 format_word) {
  const u32 f = format_word & 0x3F;
  return f == static_cast<u32>(xe::TextureFormat::k_24_8) ||
         f == static_cast<u32>(xe::TextureFormat::k_24_8_FLOAT);
}

bool DecodeHeader(u32 va, GuestSurface &out) {
  const u8 *h = mem::at<u8>(va);
  if (!h)
    return false;
  const u32 surface_info = rex::memory::load_and_swap<u32>(h + obj::kSurfaceInfo);
  const u32 info = rex::memory::load_and_swap<u32>(h + obj::kSurfaceColorInfo);
  const u32 hi = rex::memory::load_and_swap<u32>(h + obj::kSurfaceHiControl);
  const u32 size_bits = rex::memory::load_and_swap<u32>(h + obj::kSurfaceSize);
  const u32 format_word = rex::memory::load_and_swap<u32>(h + obj::kSurfaceFormat);
  const u32 width = (size_bits >> 18) + 1;
  const u32 height = ((size_bits >> 3) & 0x7FFF) + 1;
  if (width == 0 || height == 0 || width > 8192 || height > 8192)
    return false;
  out.va = va;
  out.surfaceInfo = surface_info;
  out.info = info;
  out.hiControl = hi;
  out.sizeBits = size_bits;
  out.formatWord = format_word;
  out.width = width;
  out.height = height;
  const u32 msaa = (surface_info >> 16) & 3;
  out.msaaSamples = msaa == 2 ? 4 : msaa == 1 ? 2 : 1;
  out.isDepth = IsDepthFormatWord(format_word);
  out.baseTile = info & 0xFFF;
  if (out.isDepth) {
    out.depthFormat = (info >> 16) & 1;
    out.colorFormat = 0;
    out.colorExpBias = 0;
  } else {
    out.colorFormat = (info >> 16) & 0xF;
    out.depthFormat = 0;
    const u32 bias = (info >> 20) & 0x3F;
    out.colorExpBias = bias & 0x20 ? static_cast<i32>(bias) - 64 : static_cast<i32>(bias);
  }
  return true;
}

u32 HostSampleCountFor(const VideoState &s, const GuestSurface &surf) {
  if (s.host_msaa_samples <= 1 || surf.msaaSamples != 1)
    return 1;
  if (surf.width != kGuestRenderWidth || surf.height != kGuestRenderHeight)
    return 1;
  return s.host_msaa_samples;
}

bool CreateHostTarget(VideoState &s, GuestSurface &surf) {
  HostTexture &host = surf.host;
  host.format = SurfaceHostFormat(surf);
  surf.scale = surf.isDepth && surf.width == 1024 && surf.height == 1024
                   ? ShadowMapTargetScale()
                   : RenderScaleFactor();
  host.width = ScaleDimBy(surf.width, surf.scale);
  host.height = ScaleDimBy(surf.height, surf.scale);
  host.depth = 1;
  host.mipLevels = 1;
  host.arraySize = 1;
  host.sampleCount = HostSampleCountFor(s, surf);
  host.isDepth = surf.isDepth;
  host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = host.width;
  desc.height = host.height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = host.format;
  desc.flags = surf.isDepth ? plume::RenderTextureFlag::DEPTH_TARGET
                            : plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount = static_cast<plume::RenderSampleCounts>(host.sampleCount);
  desc.committed = Settings::CommittedTextures();
  plume::RenderClearValue clear;
  if (!surf.isDepth) {
    clear = plume::RenderClearValue::Color(plume::RenderColor(0, 0, 0, 0), host.format);
    desc.optimizedClearValue = &clear;
  }
  CreateOrRecycleHostTexture(s, host, desc, surf.isDepth ? "surface-ds" : "surface-rt");
  return host.texture != nullptr;
}

}

plume::RenderFormat SurfaceHostFormat(const GuestSurface &surface) {
  return surface.isDepth ? DepthRenderTargetFormat()
                         : ConvertColorRenderTargetFormat(surface.colorFormat);
}

u64 DescriptorKey(const GuestSurface &d) {
  u64 k = d.baseTile;
  k = k * 0x9E3779B97F4A7C15ull ^ d.width;
  k = k * 0x9E3779B97F4A7C15ull ^ d.height;
  k = k * 0x9E3779B97F4A7C15ull ^ (d.isDepth ? 0x100u : 0u);
  k = k * 0x9E3779B97F4A7C15ull ^ (d.isDepth ? d.depthFormat : d.colorFormat);
  k = k * 0x9E3779B97F4A7C15ull ^ d.msaaSamples;
  return k;
}

static bool SurfaceDescriptorReferenced(const VideoState &s, u64 key) {
  for (const auto &binding : s.surface_key_by_va) {
    if (binding.second.key == key)
      return true;
  }
  return false;
}

static void OrphanSurfaceDescriptor(VideoState &s, u64 key) {
  if (!SurfaceDescriptorReferenced(s, key))
    s.orphaned_surface_frame.try_emplace(key, s.guest_frames);
}

static void TrackSurfaceDescriptor(VideoState &s, u32 surface_va, u64 key) {
  auto [binding, inserted] = s.surface_key_by_va.emplace(
      surface_va, VideoState::SurfaceHeaderBinding{key, s.guest_frames});
  s.orphaned_surface_frame.erase(key);
  if (inserted)
    return;
  const u64 old_key = binding->second.key;
  binding->second = {key, s.guest_frames};
  if (old_key == key)
    return;
  OrphanSurfaceDescriptor(s, old_key);
}

static void ForgetSurfaceDescriptor(VideoState &s, u32 surface_va) {
  const auto binding = s.surface_key_by_va.find(surface_va);
  if (binding == s.surface_key_by_va.end())
    return;
  const u64 old_key = binding->second.key;
  s.surface_key_by_va.erase(binding);
  OrphanSurfaceDescriptor(s, old_key);
}

static bool HalvesTo(u32 full, u32 half) {
  return half && (half == full / 2 || half == (full + 1) / 2);
}

GuestSurface *FindMultisampleAliasSource(VideoState &s, const GuestSurface &alias) {
  if (alias.msaaSamples < 2 || alias.drawn || !alias.width || !alias.height)
    return nullptr;
  GuestSurface *best = nullptr;
  for (auto &[key, slot] : s.surfaces) {
    GuestSurface *c = slot.get();
    if (!c || static_cast<const GuestSurface *>(c) == &alias || !c->drawn || !c->host.valid())
      continue;
    if (c->msaaSamples != 1 || c->isDepth != alias.isDepth || c->baseTile != alias.baseTile)
      continue;
    if (c->isDepth ? c->depthFormat != alias.depthFormat : c->colorFormat != alias.colorFormat)
      continue;
    if (!HalvesTo(c->width, alias.width) || !HalvesTo(c->height, alias.height))
      continue;
    if (!best || c->lastUseFrame > best->lastUseFrame)
      best = c;
  }
  return best;
}

GuestSurface *GetGuestSurface(VideoState &s, u32 surface_va) {
  if (!surface_va)
    return nullptr;
  GuestSurface decoded;
  if (!DecodeHeader(surface_va, decoded)) {
    ForgetSurfaceDescriptor(s, surface_va);
    u32 n;
    if (DiagShouldLog(0x5C00 ^ surface_va, &n))
      EOT_WARN("[surfaces] {:#x}: unreadable or absurd header", surface_va);
    return nullptr;
  }
  const u64 key = DescriptorKey(decoded);
  auto &slot = s.surfaces[key];
  if (slot && slot->host.valid()) {
    slot->va = surface_va;
    slot->surfaceInfo = decoded.surfaceInfo;
    slot->info = decoded.info;
    slot->hiControl = decoded.hiControl;
    slot->colorExpBias = decoded.colorExpBias;
    TrackSurfaceDescriptor(s, surface_va, key);
    return slot.get();
  }
  if (slot)
    ParkHostTexture(s, slot->host);
  auto surf = std::make_unique<GuestSurface>();
  *surf = std::move(decoded);
  if (!CreateHostTarget(s, *surf)) {
    slot.reset();
    s.surfaces.erase(key);
    ForgetSurfaceDescriptor(s, surface_va);
    return nullptr;
  }
  EOT_INFO("[surfaces] {:#x}: {} {}x{} fmt={} msaa={} tile={} -> host fmt {} {}x{}{}", surface_va,
           surf->isDepth ? "depth" : "color", surf->width, surf->height,
           surf->isDepth ? surf->depthFormat : surf->colorFormat, surf->msaaSamples,
           surf->baseTile, static_cast<u32>(surf->host.format), surf->host.width,
           surf->host.height,
           surf->host.sampleCount > 1 ? std::format(" {}x samples", surf->host.sampleCount) : "");
  slot = std::move(surf);
  TrackSurfaceDescriptor(s, surface_va, key);
  return slot.get();
}

void EvictStaleGuestSurfaces(VideoState &s) {
  constexpr u64 kSurfaceIdleFrames = 120;
  for (auto binding = s.surface_key_by_va.begin(); binding != s.surface_key_by_va.end();) {
    if (binding->second.lastSeenFrame + kSurfaceIdleFrames >= s.guest_frames) {
      ++binding;
      continue;
    }
    const u64 old_key = binding->second.key;
    binding = s.surface_key_by_va.erase(binding);
    OrphanSurfaceDescriptor(s, old_key);
  }

  for (auto it = s.orphaned_surface_frame.begin(); it != s.orphaned_surface_frame.end();) {
    if (SurfaceDescriptorReferenced(s, it->first)) {
      it = s.orphaned_surface_frame.erase(it);
      continue;
    }
    if (it->second >= s.guest_frames) {
      ++it;
      continue;
    }
    const auto surface = s.surfaces.find(it->first);
    if (surface != s.surfaces.end()) {
      if (surface->second)
        ParkHostTexture(s, surface->second->host);
      s.surfaces.erase(surface);
    }
    it = s.orphaned_surface_frame.erase(it);
  }
}

plume::RenderFramebuffer *GetFramebuffer(VideoState &s, HostTexture *const color[4],
                                         u32 color_count, HostTexture *depth) {
  u64 key = 0x9E3779B97F4A7C15ull;
  const plume::RenderTexture *colors[4] = {};
  u32 n = 0;
  for (u32 i = 0; i < color_count && i < 4; ++i) {
    if (!color[i] || !color[i]->texture)
      break;
    colors[n++] = color[i]->texture.get();
    key ^= reinterpret_cast<u64>(colors[i]) * (i + 1) * 0x100000001B3ull;
    key = (key << 13) | (key >> 51);
  }
  const plume::RenderTexture *ds = depth && depth->texture ? depth->texture.get() : nullptr;
  key ^= reinterpret_cast<u64>(ds) * 0xC2B2AE3D27D4EB4Full;
  if (n == 0 && !ds)
    return nullptr;
  auto it = s.framebuffers.find(key);
  if (it != s.framebuffers.end())
    return it->second.fb.get();
  plume::RenderFramebufferDesc desc(n ? colors : nullptr, n, ds);
  s.perf.host_framebuffers++;
  auto fb = s.device->createFramebuffer(desc);
  if (!fb) {
    EOT_ERROR("[surfaces] createFramebuffer failed ({} colour, depth={})", n, ds != nullptr);
    return nullptr;
  }
  auto *raw = fb.get();
  VideoState::CachedFramebuffer entry;
  entry.fb = std::move(fb);
  for (u32 i = 0; i < n; ++i)
    entry.attachments[entry.attachmentCount++] = colors[i];
  if (ds)
    entry.attachments[entry.attachmentCount++] = ds;
  s.framebuffers.emplace(key, std::move(entry));
  return raw;
}

}
