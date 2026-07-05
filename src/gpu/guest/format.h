/**
 * @file    gpu/guest/format.h
 * @brief   Guest D3D format -> plume mapping.
 *
 * Narrow-slice port: only the entries CreateTexture/CreateSurface need.
 * re:Blue's version also converts blend/stencil/cull/fill/vertex-decl enums
 * for the draw-state and shader-input paths - not needed yet.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

enum class D3DFormat : u32 {
  kA16B16G16R16F = 0x1A22AB60,
  kA16B16G16R16FAlt = 0x1A2201BF,
  kA8B8G8R8 = 0x1A200186,
  kA8R8G8B8 = 0x18280186,
  kX8R8G8B8 = 0x28280086,
  kD24FS8 = 0x1A220197,
  kD24S8 = 0x2D200196,
  kR32F = 0x2DA2ABA4,
  kG16R16F = 0x2D22AB9F,
  kG16R16FAlt = 0x2D20AB8D,
  kIndex16 = 1,
  kIndex32 = 6,
  kL8 = 0x28000102,
  kL8Alt = 0x28000002,
  kA8 = 0x04900102,
};

plume::RenderFormat ConvertGuestFormat(u32 guest_format);

bool IsRenderTargetCapable(plume::RenderFormat format);

inline bool IsDepthFormat(plume::RenderFormat format) {
  return format == plume::RenderFormat::D32_FLOAT ||
         format == plume::RenderFormat::D32_FLOAT_S8_UINT;
}

}
