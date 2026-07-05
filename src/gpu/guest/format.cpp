/**
 * @file    gpu/guest/format.cpp
 * @brief   Guest D3D format -> plume mapping.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/guest/format.h"

#include "core/logging.h"

namespace eot::gpu {

plume::RenderFormat ConvertGuestFormat(u32 guest_format) {
  switch (static_cast<D3DFormat>(guest_format)) {
  case D3DFormat::kA16B16G16R16F:
  case D3DFormat::kA16B16G16R16FAlt:
    return plume::RenderFormat::R16G16B16A16_FLOAT;
  case D3DFormat::kA8B8G8R8:
  case D3DFormat::kA8R8G8B8:
  case D3DFormat::kX8R8G8B8:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  case D3DFormat::kD24FS8:
  case D3DFormat::kD24S8:
    return plume::RenderFormat::D32_FLOAT_S8_UINT;
  case D3DFormat::kR32F:
    return plume::RenderFormat::R32_FLOAT;
  case D3DFormat::kG16R16F:
  case D3DFormat::kG16R16FAlt:
    return plume::RenderFormat::R16G16_FLOAT;
  case D3DFormat::kIndex16:
    return plume::RenderFormat::R16_UINT;
  case D3DFormat::kIndex32:
    return plume::RenderFormat::R32_UINT;
  case D3DFormat::kL8:
  case D3DFormat::kL8Alt:
  case D3DFormat::kA8:
    return plume::RenderFormat::R8_UNORM;
  default:
    EOT_ERROR(
        "ConvertGuestFormat: unknown guest format 0x{:08X}; returning UNKNOWN",
        guest_format);
    return plume::RenderFormat::UNKNOWN;
  }
}

bool IsRenderTargetCapable(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R8_UNORM:
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R32G32B32A32_FLOAT:
    return true;
  default:
    return false;
  }
}

}
