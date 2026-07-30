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

#include <rex/graphics/xenos.h>
#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

enum class D3DDeclType : u32 {
  kFloat1 = 0x2C83A4,
  kFloat2 = 0x2C23A5,
  kFloat3 = 0x2A23B9,
  kFloat4 = 0x1A23A6,
  kD3DColor = 0x182886,
  kUByte4 = 0x1A2286,
  kUByte4Alt = 0x1A2386,
  kShort2 = 0x2C2359,
  kShort4 = 0x1A235A,
  kUByte4N = 0x1A2086,
  kUByte4NAlt = 0x1A2186,
  kShort2N = 0x2C2159,
  kShort4N = 0x1A215A,
  kUShort2N = 0x2C2059,
  kUShort4N = 0x1A205A,
  kUInt1 = 0x2C82A1,
  kUDec3 = 0x2A2287,
  kDec3N = 0x2A2187,
  kDec3NAlt = 0x2A2190,
  kDec3NAlt2 = 0x2A2390,
  kDec3NWide = 0x1A2187,
  kFloat16_2 = 0x2C235F,
  kFloat16_4 = 0x1A2360,
  kUnused = 0xFFFFFFFF,
};

plume::RenderFormat ConvertDeclType(u32 decl_type);

inline constexpr u32 kGuestFormatMask = 0x3F;

plume::RenderFormat ConvertGuestFormat(u32 guest_format);

plume::RenderStencilOp ConvertStencilOp(rex::graphics::xenos::StencilOp op);

plume::RenderComparisonFunction
ConvertCompareFunc(rex::graphics::xenos::CompareFunction f);

plume::RenderComparisonFunction
ConvertDepthCompareFunc(rex::graphics::xenos::CompareFunction f,
                        bool reverse_z);

inline constexpr u32 kBlendColorSrcShift = 0;
inline constexpr u32 kBlendColorCombShift = 5;
inline constexpr u32 kBlendColorDstShift = 8;
inline constexpr u32 kBlendAlphaSrcShift = 16;
inline constexpr u32 kBlendAlphaCombShift = 21;
inline constexpr u32 kBlendAlphaDstShift = 24;

plume::RenderBlend ConvertBlendMode(u32 factor);
plume::RenderBlendOperation ConvertBlendOp(u32 op);

bool IsRenderTargetCapable(plume::RenderFormat format);

inline bool IsDepthFormat(plume::RenderFormat format) {
  return format == plume::RenderFormat::D32_FLOAT ||
         format == plume::RenderFormat::D32_FLOAT_S8_UINT;
}

}
