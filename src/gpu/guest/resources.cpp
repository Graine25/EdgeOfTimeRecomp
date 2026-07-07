#include "gpu/guest/resources.h"

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

u32 ComputeTexturePitch(const GuestTexture *tex) {
  if (!tex || !tex->width)
    return 0;
  const u32 bpt = BytesPerTexel(tex->format);
  if (!bpt)
    return 0;
  constexpr u32 kPitchAlignment = 256;
  const u32 row = tex->width * bpt;
  return (row + kPitchAlignment - 1) & ~(kPitchAlignment - 1);
}

}
