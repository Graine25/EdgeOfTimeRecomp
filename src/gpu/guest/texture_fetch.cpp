#include <array>
#include <unordered_map>
#include "gpu/guest/texture_fetch.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/guest/d3d.h"

namespace eot::gpu {

namespace xenos = rex::graphics::xenos;

namespace {

std::mutex g_census_mutex;
std::map<std::tuple<u32, u32, u32>, u32> g_shapes;
std::atomic<u32> g_decoded{0};
std::atomic<u32> g_tiled{0};
std::atomic<u32> g_mipped{0};

}

bool DecodeTextureFetch(u32 device_va, u32 sampler, GuestTextureFetch &out) {
  if (!device_va || sampler >= kMaxSamplerSlots)
    return false;
  if (!DecodeTextureFetchAt(device_va + kTextureFetchConstants +
                            sampler * kTextureFetchStride, out))
    return false;
  out.physicalAddress = true;
  return true;
}

plume::RenderFormat
HostFormatForTextureFormat(xenos::TextureFormat format) {
  switch (format) {
  case xenos::TextureFormat::k_DXT1:
    return plume::RenderFormat::BC1_UNORM;
  case xenos::TextureFormat::k_DXT2_3:
    return plume::RenderFormat::BC2_UNORM;
  case xenos::TextureFormat::k_DXT4_5:
    return plume::RenderFormat::BC3_UNORM;
  case xenos::TextureFormat::k_8:
    return plume::RenderFormat::R8_UNORM;
  case xenos::TextureFormat::k_8_8:
    return plume::RenderFormat::R8G8_UNORM;
  case xenos::TextureFormat::k_8_8_8_8:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  case xenos::TextureFormat::k_24_8:
  case xenos::TextureFormat::k_24_8_FLOAT:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  case xenos::TextureFormat::k_16_16_16_16:
    return plume::RenderFormat::R16G16B16A16_UNORM;
  case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    return plume::RenderFormat::R16G16B16A16_FLOAT;

  case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
    return plume::RenderFormat::BC1_UNORM;
  case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
    return plume::RenderFormat::BC2_UNORM;
  case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
    return plume::RenderFormat::BC3_UNORM;

  case xenos::TextureFormat::k_DXN:
    return plume::RenderFormat::BC5_UNORM;
  case xenos::TextureFormat::k_DXT5A:
    return plume::RenderFormat::BC4_UNORM;

  case xenos::TextureFormat::k_16:
    return plume::RenderFormat::R16_UNORM;
  case xenos::TextureFormat::k_16_16:
    return plume::RenderFormat::R16G16_UNORM;
  case xenos::TextureFormat::k_16_FLOAT:
    return plume::RenderFormat::R16_FLOAT;
  case xenos::TextureFormat::k_16_16_FLOAT:
    return plume::RenderFormat::R16G16_FLOAT;
  case xenos::TextureFormat::k_32_FLOAT:
    return plume::RenderFormat::R32_FLOAT;
  case xenos::TextureFormat::k_32_32_FLOAT:
    return plume::RenderFormat::R32G32_FLOAT;
  default:
    return plume::RenderFormat::UNKNOWN;
  }
}

u32 PhysicalTextureKey(u32 address, bool already_physical) {
  if (!address || already_physical)
    return address;
  constexpr u32 kPhysicalMask = 0x1FFFFFFFu;
  constexpr u32 kPageOffset = 0x1000u;
  return (address & kPhysicalMask) + kPageOffset;
}

bool DecodeTextureFetchWords(const u32 *words, GuestTextureFetch &out) {
  if (!words)
    return false;
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, words, 6 * sizeof(u32));

  if (fetch.type != xenos::FetchConstantType::kTexture)
    return false;
  if (!fetch.base_address)
    return false;

  out = GuestTextureFetch{};
  out.baseAddress = fetch.base_address << 12;
  out.mipAddress = fetch.mip_address << 12;
  out.tiled = fetch.tiled != 0;
  out.pitchTiles = fetch.pitch;
  out.pitch = out.tiled ? 0 : (fetch.pitch << 5);
  out.packedMips = fetch.packed_mips != 0;
  out.format = fetch.format;
  out.clampX = fetch.clamp_x;
  out.clampY = fetch.clamp_y;
  out.clampZ = fetch.clamp_z;
  out.magFilter = fetch.mag_filter;
  out.minFilter = fetch.min_filter;
  out.mipFilter = fetch.mip_filter;
  out.borderWhite = fetch.border_size != 0;
  out.endianness = fetch.endianness;
  out.dimension = fetch.dimension;
  out.expAdjust = fetch.exp_adjust;

  switch (fetch.dimension) {
  case xenos::DataDimension::k1D:
    out.width = fetch.size_1d.width + 1;
    out.height = 1;
    break;
  case xenos::DataDimension::k3D:
    out.width = fetch.size_3d.width + 1;
    out.height = fetch.size_3d.height + 1;
    out.depth = fetch.size_3d.depth + 1;
    break;
  case xenos::DataDimension::k2DOrStacked:
  case xenos::DataDimension::kCube:
  default:
    out.width = fetch.size_2d.width + 1;
    out.height = fetch.size_2d.height + 1;
    break;
  }

  out.mipLevels = fetch.mip_max_level + 1;
  return true;
}

void NoteTextureFetch(const GuestTextureFetch &fetch) {
  g_decoded.fetch_add(1, std::memory_order_relaxed);
  if (fetch.tiled)
    g_tiled.fetch_add(1, std::memory_order_relaxed);
  if (fetch.mipLevels > 1)
    g_mipped.fetch_add(1, std::memory_order_relaxed);

  std::lock_guard lock(g_census_mutex);
  if (g_shapes.size() < 4096) {
    ++g_shapes[{static_cast<u32>(fetch.format), fetch.width, fetch.height}];
  }
}

void LogTextureFetchCensus() {
  std::lock_guard lock(g_census_mutex);
  EOT_INFO("[texfetch] {} decoded ({} tiled, {} with mips); {} distinct shapes",
           g_decoded.load(), g_tiled.load(), g_mipped.load(), g_shapes.size());

  std::vector<std::pair<std::tuple<u32, u32, u32>, u32>> ranked(g_shapes.begin(),
                                                               g_shapes.end());
  std::sort(ranked.begin(), ranked.end(),
            [](const auto &a, const auto &b) { return a.second > b.second; });
  for (size_t i = 0; i < ranked.size() && i < 10; ++i) {
    const auto &[format, width, height] = ranked[i].first;
    EOT_INFO("[texfetch]   fmt={} {}x{} - {} binds", format, width, height,
             ranked[i].second);
  }
}

bool DecodeTextureFetchAt(u32 fetch_va, GuestTextureFetch &out) {
  if (!fetch_va)
    return false;
  u32 words[6];
  for (u32 i = 0; i < 6; ++i)
    words[i] = mem::try_load<u32>(fetch_va + i * 4);
  return DecodeTextureFetchWords(words, out);
}

namespace {

std::mutex g_canonical_mutex;
std::unordered_map<u32, std::array<u32, 6>> g_canonical;
std::atomic<u32> g_canon_registered{0};
std::atomic<u32> g_canon_served{0};
std::atomic<u32> g_canon_missed{0};

bool ReadFetchWords(u32 fetch_va, std::array<u32, 6> &out) {
  if (!fetch_va)
    return false;
  for (u32 i = 0; i < 6; ++i)
    out[i] = mem::try_load<u32>(fetch_va + i * 4);
  return out[0] != 0 || out[1] != 0 || out[2] != 0;
}

}

void RegisterCanonicalTexture(u32 texture_va, bool replace) {
  if (texture_va < 0x1000)
    return;
  std::array<u32, 6> words{};
  if (!ReadFetchWords(texture_va + kTextureObjectFetchOffset, words))
    return;
  std::lock_guard lock(g_canonical_mutex);
  auto it = g_canonical.find(texture_va);
  if (it == g_canonical.end() || replace) {
    g_canonical[texture_va] = words;
    g_canon_registered.fetch_add(1, std::memory_order_relaxed);
  }
}

void RegisterCanonicalTexturePool(u32 record_va) {
  if (record_va < 0x1000)
    return;
  for (u32 i = 0; i < 35; ++i)
    RegisterCanonicalTexture(record_va + 0x10 + i * 0x34, true);
}

void RetireCanonicalTexture(u32 texture_va) {
  std::lock_guard lock(g_canonical_mutex);
  g_canonical.erase(texture_va);
}

bool DecodeTextureObjectFetch(u32 texture_va, GuestTextureFetch &out) {
  if (!texture_va)
    return false;
  const u32 fetch_va = texture_va + kTextureObjectFetchOffset;

  std::array<u32, 6> canonical{};
  {
    std::lock_guard lock(g_canonical_mutex);
    auto it = g_canonical.find(texture_va);
    if (it == g_canonical.end()) {
      g_canon_missed.fetch_add(1, std::memory_order_relaxed);
      return DecodeTextureFetchAt(fetch_va, out);
    }
    canonical = it->second;
  }

  std::array<u32, 6> live{};
  if (ReadFetchWords(fetch_va, live)) {
    canonical[1] = (canonical[1] & 0xFFFu) | (live[1] & 0xFFFFF000u);
    canonical[5] = (canonical[5] & 0xFFFu) | (live[5] & 0xFFFFF000u);
  }
  g_canon_served.fetch_add(1, std::memory_order_relaxed);
  return DecodeTextureFetchWords(canonical.data(), out);
}

void LogCanonicalTextureStats() {
  EOT_INFO("[canon] {} texture objects registered; {} fetches served from the "
           "canonical header, {} decoded live",
           g_canon_registered.load(), g_canon_served.load(),
           g_canon_missed.load());
}

}
