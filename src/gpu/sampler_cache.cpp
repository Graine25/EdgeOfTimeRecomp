#include "gpu/sampler_cache.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/settings.h"

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;

struct DescKey {
  plume::RenderTextureAddressMode u, v, w;
  plume::RenderFilter min, mag;
  plume::RenderMipmapMode mip;
  plume::RenderBorderColor border;
  u32 maxAniso;
  bool anisoEnabled;
  float lodBias;
  float minLod;
  float maxLod;

  bool operator==(const DescKey &o) const noexcept {
    return u == o.u && v == o.v && w == o.w && min == o.min && mag == o.mag && mip == o.mip &&
           border == o.border && maxAniso == o.maxAniso && anisoEnabled == o.anisoEnabled &&
           lodBias == o.lodBias && minLod == o.minLod && maxLod == o.maxLod;
  }
};

struct DescKeyHash {
  size_t operator()(const DescKey &k) const noexcept {
    u64 bits = 0;
    bits |= static_cast<u64>(k.u);
    bits |= static_cast<u64>(k.v) << 4;
    bits |= static_cast<u64>(k.w) << 8;
    bits |= static_cast<u64>(k.min) << 12;
    bits |= static_cast<u64>(k.mag) << 16;
    bits |= static_cast<u64>(k.mip) << 20;
    bits |= static_cast<u64>(k.border) << 24;
    bits |= static_cast<u64>(k.maxAniso) << 28;
    bits |= static_cast<u64>(k.anisoEnabled) << 36;
    u32 lb, mn, mx;
    std::memcpy(&lb, &k.lodBias, 4);
    std::memcpy(&mn, &k.minLod, 4);
    std::memcpy(&mx, &k.maxLod, 4);
    return std::hash<u64>{}(bits ^ (u64(lb) << 13) ^ (u64(mn) << 29) ^ (u64(mx) << 41));
  }
};

struct Cache {
  std::unordered_map<DescKey, std::pair<std::unique_ptr<plume::RenderSampler>, u32>, DescKeyHash>
      map;
};

Cache &cache() {
  static Cache c;
  return c;
}

plume::RenderTextureAddressMode ConvertClamp(xe::ClampMode mode) {
  using A = plume::RenderTextureAddressMode;
  switch (mode) {
  case xe::ClampMode::kRepeat:
    return A::WRAP;
  case xe::ClampMode::kMirroredRepeat:
    return A::MIRROR;
  case xe::ClampMode::kClampToEdge:
  case xe::ClampMode::kClampToHalfway:
    return A::CLAMP;
  case xe::ClampMode::kMirrorClampToEdge:
  case xe::ClampMode::kMirrorClampToHalfway:
  case xe::ClampMode::kMirrorClampToBorder:
    return A::MIRROR_ONCE;
  case xe::ClampMode::kClampToBorder:
    return A::BORDER;
  }
  return A::CLAMP;
}

}

plume::RenderSamplerDesc DecodeSamplerFromFetch(const u32 fc[6], bool mipmapped_upload) {
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fc, sizeof(fetch));

  plume::RenderSamplerDesc d;
  d.addressU = ConvertClamp(fetch.clamp_x);
  d.addressV = ConvertClamp(fetch.clamp_y);
  d.addressW = ConvertClamp(fetch.clamp_z);
  const bool mag_point = fetch.mag_filter == xe::TextureFilter::kPoint;
  const bool min_point = fetch.min_filter == xe::TextureFilter::kPoint;
  const bool mip_point = fetch.mip_filter == xe::TextureFilter::kPoint ||
                         fetch.mip_filter == xe::TextureFilter::kBaseMap;
  d.magFilter = mag_point ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  d.minFilter = min_point ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
  d.mipmapMode = mip_point ? plume::RenderMipmapMode::NEAREST : plume::RenderMipmapMode::LINEAR;
  if (fetch.mip_filter == xe::TextureFilter::kBaseMap) {
    d.maxLOD = 0.0f;
  }
  u32 aniso = 0;
  switch (fetch.aniso_filter) {
  case xe::AnisoFilter::kMax_2_1:
    aniso = 2;
    break;
  case xe::AnisoFilter::kMax_4_1:
    aniso = 4;
    break;
  case xe::AnisoFilter::kMax_8_1:
    aniso = 8;
    break;
  case xe::AnisoFilter::kMax_16_1:
    aniso = 16;
    break;
  default:
    break;
  }
  const i32 forced = Settings::Anisotropy();
  if (forced == 1)
    aniso = 0;
  else if (forced > 1 && !mag_point && !min_point && mipmapped_upload)
    aniso = std::max<u32>(aniso, std::min<u32>(static_cast<u32>(forced), 16u));
  const bool aniso_on = aniso > 1 && !mag_point && !min_point;
  d.anisotropyEnabled = aniso_on;
  d.maxAnisotropy = aniso_on ? aniso : 1u;
  if (aniso_on && fetch.mip_filter != xe::TextureFilter::kBaseMap)
    d.mipmapMode = plume::RenderMipmapMode::LINEAR;
  d.borderColor = fetch.border_color == xe::BorderColor::k_ABGR_White
                      ? plume::RenderBorderColor::OPAQUE_WHITE
                      : plume::RenderBorderColor::TRANSPARENT_BLACK;
  d.mipLODBias = static_cast<float>(fetch.lod_bias) / 32.0f;
  d.minLOD = static_cast<float>(fetch.mip_min_level);
  if (d.maxLOD != 0.0f)
    d.maxLOD = static_cast<float>(fetch.mip_max_level);
  return d;
}

u32 ResolveSamplerSlotLocked(const plume::RenderSamplerDesc &desc) {
  auto &s = state();
  if (!s.ready || !s.device || !s.sampler_descriptor_set)
    return kSamplerLinearClamp;

  const DescKey key{desc.addressU,    desc.addressV,      desc.addressW,
                    desc.minFilter,   desc.magFilter,     desc.mipmapMode,
                    desc.borderColor, desc.maxAnisotropy, desc.anisotropyEnabled,
                    desc.mipLODBias,  desc.minLOD,        desc.maxLOD};
  auto &c = cache();
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.second;

  u32 slot = kInvalidDescriptorIndex;
  for (size_t i = kReservedSamplerCount; i < s.sampler_descriptor_used.size(); ++i) {
    if (!s.sampler_descriptor_used[i]) {
      s.sampler_descriptor_used[i] = true;
      slot = static_cast<u32>(i);
      break;
    }
  }
  if (slot == kInvalidDescriptorIndex) {
    u32 n;
    if (DiagShouldLog(0x5A01, &n))
      EOT_WARN("[sampler-cache] bindless sampler heap full, falling back to slot 0");
    return kSamplerLinearClamp;
  }
  auto sampler = s.device->createSampler(desc);
  s.sampler_descriptor_set->setSampler(slot, sampler.get());
  c.map.emplace(key, std::make_pair(std::move(sampler), slot));
  static u32 logged = 0;
  if (logged++ < 48)
    EOT_DEBUG("[sampler] #{} slot {}: min {} mag {} mip {} aniso {}x{} lod bias {:g} lod {:g}..{:g} "
             "address {}/{}/{}",
             logged, slot, static_cast<int>(desc.minFilter), static_cast<int>(desc.magFilter),
             static_cast<int>(desc.mipmapMode), desc.anisotropyEnabled ? 1 : 0,
             desc.maxAnisotropy, desc.mipLODBias, desc.minLOD, desc.maxLOD,
             static_cast<int>(desc.addressU), static_cast<int>(desc.addressV),
             static_cast<int>(desc.addressW));
  return slot;
}

}
