#include "gpu/surfaces.h"

#include <memory>

#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;

bool IsDepthFormatWord(u32 format_word) {
  const u32 f = format_word & 0x3F;
  return f == static_cast<u32>(xe::TextureFormat::k_24_8) ||
         f == static_cast<u32>(xe::TextureFormat::k_24_8_FLOAT);
}

bool DecodeHeader(u32 va, GuestSurface &out) {
  const u32 surface_info = mem::load<u32>(va + obj::kSurfaceInfo);
  const u32 info = mem::load<u32>(va + obj::kSurfaceColorInfo);
  const u32 hi = mem::load<u32>(va + obj::kSurfaceHiControl);
  const u32 size_bits = mem::load<u32>(va + obj::kSurfaceSize);
  const u32 format_word = mem::load<u32>(va + obj::kSurfaceFormat);
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

bool CreateHostTarget(VideoState &s, GuestSurface &surf) {
  HostTexture &host = surf.host;
  host.format = SurfaceHostFormat(surf);
  host.width = surf.width;
  host.height = surf.height;
  host.depth = 1;
  host.mipLevels = 1;
  host.arraySize = 1;
  host.sampleCount = 1;
  host.isDepth = surf.isDepth;
  host.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = surf.width;
  desc.height = surf.height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = host.format;
  desc.flags = surf.isDepth ? plume::RenderTextureFlag::DEPTH_TARGET
                            : plume::RenderTextureFlag::RENDER_TARGET;
  desc.committed = true;
  plume::RenderClearValue clear;
  if (!surf.isDepth) {
    clear = plume::RenderClearValue::Color(plume::RenderColor(0, 0, 0, 0), host.format);
    desc.optimizedClearValue = &clear;
  }
  host.texture = CreateHostTexture(s.device.get(), desc, surf.isDepth ? "surface-ds" : "surface-rt");
  if (!host.texture)
    return false;
  host.layout = plume::RenderTextureLayout::UNKNOWN;
  return true;
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

GuestSurface *GetGuestSurface(VideoState &s, u32 surface_va) {
  if (!surface_va)
    return nullptr;
  GuestSurface decoded;
  if (!DecodeHeader(surface_va, decoded)) {
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
    return slot.get();
  }
  if (slot)
    ParkHostTexture(s, slot->host);
  auto surf = std::make_unique<GuestSurface>();
  *surf = std::move(decoded);
  if (!CreateHostTarget(s, *surf)) {
    slot.reset();
    return nullptr;
  }
  EOT_INFO("[surfaces] {:#x}: {} {}x{} fmt={} msaa={} tile={} -> host fmt {}", surface_va,
           surf->isDepth ? "depth" : "color", surf->width, surf->height,
           surf->isDepth ? surf->depthFormat : surf->colorFormat, surf->msaaSamples,
           surf->baseTile, static_cast<u32>(surf->host.format));
  slot = std::move(surf);
  return slot.get();
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
    return it->second.get();
  plume::RenderFramebufferDesc desc(n ? colors : nullptr, n, ds);
  auto fb = s.device->createFramebuffer(desc);
  if (!fb) {
    EOT_ERROR("[surfaces] createFramebuffer failed ({} colour, depth={})", n, ds != nullptr);
    return nullptr;
  }
  auto *raw = fb.get();
  s.framebuffers.emplace(key, std::move(fb));
  return raw;
}

}
