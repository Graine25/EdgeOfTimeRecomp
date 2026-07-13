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

bool DecodeTextureFetchAt(u32 fetch_va, GuestTextureFetch &out) {
  if (!fetch_va)
    return false;

  const u32 base = fetch_va;
  u32 words[6];
  for (u32 i = 0; i < 6; ++i)
    words[i] = mem::try_load<u32>(base + i * 4);

  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, words, sizeof(words));

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
  out.endianness = fetch.endianness;
  out.dimension = fetch.dimension;

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

}
