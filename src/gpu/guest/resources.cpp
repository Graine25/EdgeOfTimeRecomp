#include "gpu/guest/resources.h"

#include <algorithm>

namespace eot::gpu {

u32 BytesPerTexel(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R8_UNORM:
    return 1;
  case plume::RenderFormat::R8G8_UNORM:
  case plume::RenderFormat::R16_UNORM:
  case plume::RenderFormat::R16_FLOAT:
    return 2;
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R32_FLOAT:
  case plume::RenderFormat::D32_FLOAT:
  case plume::RenderFormat::D32_FLOAT_S8_UINT:
    return 4;
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R32G32_FLOAT:
    return 8;
  case plume::RenderFormat::R32G32B32A32_FLOAT:
    return 16;
  default:
    return 0;
  }
}

u32 BytesPerBlock(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::BC1_UNORM:
  case plume::RenderFormat::BC1_UNORM_SRGB:
  case plume::RenderFormat::BC4_UNORM:
  case plume::RenderFormat::BC4_SNORM:
    return 8;
  case plume::RenderFormat::BC2_UNORM:
  case plume::RenderFormat::BC2_UNORM_SRGB:
  case plume::RenderFormat::BC3_UNORM:
  case plume::RenderFormat::BC3_UNORM_SRGB:
  case plume::RenderFormat::BC5_UNORM:
  case plume::RenderFormat::BC5_SNORM:
  case plume::RenderFormat::BC7_UNORM:
  case plume::RenderFormat::BC7_UNORM_SRGB:
    return 16;
  default:
    return 0;
  }
}

bool IsBlockCompressed(plume::RenderFormat format) {
  return BytesPerBlock(format) != 0;
}

TextureFootprint ComputeTextureFootprint(const GuestTexture *tex, u32 level) {
  TextureFootprint fp;
  if (!tex || !tex->width || !tex->height)
    return fp;

  const u32 width = std::max(1u, tex->width >> level);
  const u32 height = std::max(1u, tex->height >> level);

  constexpr u32 kPitchAlignment = 256; // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
  const u32 block_bytes = BytesPerBlock(tex->format);
  u32 row = 0;
  if (block_bytes) {
    fp.blockSize = kTextureBlockSize;
    fp.unitBytes = block_bytes;
    fp.rowUnits = (width + kTextureBlockSize - 1) / kTextureBlockSize;
    fp.rows = (height + kTextureBlockSize - 1) / kTextureBlockSize;
    row = fp.rowUnits * block_bytes;
  } else {
    const u32 bpt = BytesPerTexel(tex->format);
    if (!bpt)
      return {};
    fp.blockSize = 1;
    fp.unitBytes = bpt;
    fp.rowUnits = width;
    fp.rows = height;
    row = width * bpt;
  }
  fp.pitch = (row + kPitchAlignment - 1) & ~(kPitchAlignment - 1);
  return fp;
}

u32 ComputeTexturePitch(const GuestTexture *tex) {
  return ComputeTextureFootprint(tex).pitch;
}

}
