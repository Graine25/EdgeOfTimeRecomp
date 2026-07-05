/**
 * @file    gpu/guest/format.h
 * @brief   Guest D3D format -> plume mapping.
 *
 * Narrow-slice port: only what CreateTexture/CreateSurface need. re:Blue's
 * version also converts blend/stencil/cull/fill/vertex-decl enums for the
 * draw-state and shader-input paths - not needed yet.
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

inline constexpr u32 kGuestFormatMask = 0x3F;

plume::RenderFormat ConvertGuestFormat(u32 guest_format);

bool IsRenderTargetCapable(plume::RenderFormat format);

inline bool IsDepthFormat(plume::RenderFormat format) {
  return format == plume::RenderFormat::D32_FLOAT ||
         format == plume::RenderFormat::D32_FLOAT_S8_UINT;
}

}
