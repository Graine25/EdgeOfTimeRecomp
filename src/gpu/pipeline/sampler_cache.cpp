#include "gpu/pipeline/sampler_cache.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "gpu/device/device.h"

namespace eot::gpu::samplers {

namespace {

namespace xenos = rex::graphics::xenos;

std::mutex g_mutex;
std::unordered_map<u64, u32> g_slots;
std::atomic<u32> g_created{0};
std::atomic<u32> g_reused{0};
std::atomic<u32> g_exhausted{0};

plume::RenderTextureAddressMode ConvertAddress(xenos::ClampMode mode) {
  switch (mode) {
  case xenos::ClampMode::kRepeat:
    return plume::RenderTextureAddressMode::WRAP;
  case xenos::ClampMode::kMirroredRepeat:
    return plume::RenderTextureAddressMode::MIRROR;
  case xenos::ClampMode::kClampToEdge:
    return plume::RenderTextureAddressMode::CLAMP;
  case xenos::ClampMode::kMirrorClampToEdge:
    return plume::RenderTextureAddressMode::MIRROR_ONCE;
  case xenos::ClampMode::kClampToBorder:
    return plume::RenderTextureAddressMode::BORDER;
  default:
    return plume::RenderTextureAddressMode::CLAMP;
  }
}

plume::RenderFilter ConvertFilter(xenos::TextureFilter filter) {
  return filter == xenos::TextureFilter::kLinear ? plume::RenderFilter::LINEAR
                                                 : plume::RenderFilter::NEAREST;
}

plume::RenderMipmapMode ConvertMipFilter(xenos::TextureFilter filter) {
  return filter == xenos::TextureFilter::kLinear
             ? plume::RenderMipmapMode::LINEAR
             : plume::RenderMipmapMode::NEAREST;
}

}

u32 ResolveSlot(const GuestTextureFetch &fetch, bool force_clamp) {
  const auto address_u = force_clamp ? plume::RenderTextureAddressMode::CLAMP
                                     : ConvertAddress(fetch.clampX);
  const auto address_v = force_clamp ? plume::RenderTextureAddressMode::CLAMP
                                     : ConvertAddress(fetch.clampY);
  const auto address_w = force_clamp ? plume::RenderTextureAddressMode::CLAMP
                                     : ConvertAddress(fetch.clampZ);
  const auto mag = ConvertFilter(fetch.magFilter);
  const auto min = ConvertFilter(fetch.minFilter);
  const auto mip = ConvertMipFilter(fetch.mipFilter);
  const auto border = fetch.borderWhite
                          ? plume::RenderBorderColor::OPAQUE_WHITE
                          : plume::RenderBorderColor::TRANSPARENT_BLACK;

  const u64 key = (u64(address_u) << 0) | (u64(address_v) << 4) |
                  (u64(address_w) << 8) | (u64(mag) << 12) |
                  (u64(min) << 16) | (u64(mip) << 20) | (u64(border) << 24);

  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_slots.find(key); it != g_slots.end()) {
      g_reused.fetch_add(1, std::memory_order_relaxed);
      return it->second;
    }
  }

  plume::RenderSamplerDesc desc;
  desc.addressU = address_u;
  desc.addressV = address_v;
  desc.addressW = address_w;
  desc.magFilter = mag;
  desc.minFilter = min;
  desc.mipmapMode = mip;
  desc.borderColor = border;

  const u32 slot = Video::AcquireSamplerDescriptor(desc);
  if (slot == kInvalidDescriptorIndex) {
    if (g_exhausted.fetch_add(1, std::memory_order_relaxed) == 0)
      EOT_WARN("[samplers] heap exhausted; falling back to the default sampler");
    return 0;
  }

  std::lock_guard lock(g_mutex);
  g_slots.emplace(key, slot);
  g_created.fetch_add(1, std::memory_order_relaxed);
  return slot;
}

void LogStats() {
  EOT_INFO("[samplers] {} distinct samplers, {} bindings reused, {} past the "
           "end of the heap",
           g_created.load(), g_reused.load(), g_exhausted.load());
}

void Shutdown() {
  std::lock_guard lock(g_mutex);
  g_slots.clear();
}

}
