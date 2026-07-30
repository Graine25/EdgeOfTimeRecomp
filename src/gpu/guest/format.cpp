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

#include <atomic>
#include <mutex>
#include <unordered_set>

#include <rex/graphics/xenos.h>

#include "core/logging.h"

namespace eot::gpu {
namespace {

namespace xe = rex::graphics::xenos;

void WarnUnmapped(u32 guest_format, u32 base) {
  static std::mutex mutex;
  static std::unordered_set<u32> seen;
  std::lock_guard lock(mutex);
  if (seen.insert(base).second) {
    EOT_ERROR("ConvertGuestFormat: unmapped Xenos base format {} (from guest "
              "format 0x{:08X}); returning UNKNOWN",
              base, guest_format);
  }
}

}

plume::RenderFormat ConvertGuestFormat(u32 guest_format) {
  const u32 base = guest_format & kGuestFormatMask;
  switch (static_cast<xe::TextureFormat>(base)) {
  case xe::TextureFormat::k_DXT1:
    return plume::RenderFormat::BC1_UNORM;
  case xe::TextureFormat::k_DXT2_3:
    return plume::RenderFormat::BC2_UNORM;
  case xe::TextureFormat::k_DXT4_5:
    return plume::RenderFormat::BC3_UNORM;
  case xe::TextureFormat::k_DXT5A:
    return plume::RenderFormat::BC4_UNORM;
  case xe::TextureFormat::k_DXN:
    return plume::RenderFormat::BC5_UNORM;

  case xe::TextureFormat::k_8:
  case xe::TextureFormat::k_8_A:
  case xe::TextureFormat::k_8_B:
    return plume::RenderFormat::R8_UNORM;
  case xe::TextureFormat::k_8_8:
    return plume::RenderFormat::R8G8_UNORM;
  case xe::TextureFormat::k_8_8_8_8:
  case xe::TextureFormat::k_8_8_8_8_A:
  case xe::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
    return plume::RenderFormat::R8G8B8A8_UNORM;

  case xe::TextureFormat::k_2_10_10_10:
  case xe::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
    return plume::RenderFormat::R16G16B16A16_FLOAT;

  case xe::TextureFormat::k_16:
    return plume::RenderFormat::R16_UNORM;
  case xe::TextureFormat::k_16_16:
    return plume::RenderFormat::R16G16_UNORM;
  case xe::TextureFormat::k_16_16_16_16:
    return plume::RenderFormat::R16G16B16A16_UNORM;

  case xe::TextureFormat::k_16_16_EDRAM:
    return plume::RenderFormat::R16G16_FLOAT;
  case xe::TextureFormat::k_16_16_16_16_EDRAM:
    return plume::RenderFormat::R16G16B16A16_FLOAT;
  case xe::TextureFormat::k_16_FLOAT:
    return plume::RenderFormat::R16_FLOAT;
  case xe::TextureFormat::k_16_16_FLOAT:
    return plume::RenderFormat::R16G16_FLOAT;
  case xe::TextureFormat::k_16_16_16_16_FLOAT:
    return plume::RenderFormat::R16G16B16A16_FLOAT;
  case xe::TextureFormat::k_32_FLOAT:
    return plume::RenderFormat::R32_FLOAT;
  case xe::TextureFormat::k_32_32_FLOAT:
    return plume::RenderFormat::R32G32_FLOAT;
  case xe::TextureFormat::k_32_32_32_32_FLOAT:
    return plume::RenderFormat::R32G32B32A32_FLOAT;

  case xe::TextureFormat::k_24_8:
  case xe::TextureFormat::k_24_8_FLOAT:
    return plume::RenderFormat::D32_FLOAT_S8_UINT;

  default:
    WarnUnmapped(guest_format, base);
    return plume::RenderFormat::UNKNOWN;
  }
}

bool IsRenderTargetCapable(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R8_UNORM:
  case plume::RenderFormat::R8G8_UNORM:
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16_UNORM:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R16_FLOAT:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R32_FLOAT:
  case plume::RenderFormat::R32G32_FLOAT:
  case plume::RenderFormat::R32G32B32A32_FLOAT:
    return true;
  default:
    return false;
  }
}

plume::RenderFormat ConvertDeclType(u32 decl_type) {
  switch (static_cast<D3DDeclType>(decl_type)) {
  case D3DDeclType::kFloat1:
    return plume::RenderFormat::R32_FLOAT;
  case D3DDeclType::kFloat2:
    return plume::RenderFormat::R32G32_FLOAT;
  case D3DDeclType::kFloat3:
    return plume::RenderFormat::R32G32B32_FLOAT;
  case D3DDeclType::kFloat4:
    return plume::RenderFormat::R32G32B32A32_FLOAT;
  case D3DDeclType::kD3DColor:
    return plume::RenderFormat::B8G8R8A8_UNORM;
  case D3DDeclType::kUByte4:
  case D3DDeclType::kUByte4Alt:
    return plume::RenderFormat::R8G8B8A8_UINT;
  case D3DDeclType::kShort2:
    return plume::RenderFormat::R16G16_SINT;
  case D3DDeclType::kShort4:
    return plume::RenderFormat::R16G16B16A16_SNORM;
  case D3DDeclType::kUByte4N:
  case D3DDeclType::kUByte4NAlt:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  case D3DDeclType::kShort2N:
    return plume::RenderFormat::R16G16_SNORM;
  case D3DDeclType::kShort4N:
    return plume::RenderFormat::R16G16B16A16_SNORM;
  case D3DDeclType::kUShort2N:
    return plume::RenderFormat::R16G16_UNORM;
  case D3DDeclType::kUShort4N:
    return plume::RenderFormat::R16G16B16A16_UNORM;
  case D3DDeclType::kUInt1:
    return plume::RenderFormat::R32_UINT;
  case D3DDeclType::kUDec3:
  case D3DDeclType::kDec3N:
  case D3DDeclType::kDec3NAlt:
  case D3DDeclType::kDec3NAlt2:
  case D3DDeclType::kDec3NWide:
    return plume::RenderFormat::R32_UINT;
  case D3DDeclType::kFloat16_2:
    return plume::RenderFormat::R16G16_FLOAT;
  case D3DDeclType::kFloat16_4:
    return plume::RenderFormat::R16G16B16A16_FLOAT;
  default:
    return plume::RenderFormat::UNKNOWN;
  }
}

plume::RenderStencilOp ConvertStencilOp(rex::graphics::xenos::StencilOp op) {
  using SO = rex::graphics::xenos::StencilOp;
  using RS = plume::RenderStencilOp;
  switch (op) {
  case SO::kZero:
    return RS::ZERO;
  case SO::kReplace:
    return RS::REPLACE;
  case SO::kIncrementClamp:
    return RS::INCREMENT_AND_CLAMP;
  case SO::kDecrementClamp:
    return RS::DECREMENT_AND_CLAMP;
  case SO::kInvert:
    return RS::INVERT;
  case SO::kIncrementWrap:
    return RS::INCREMENT_AND_WRAP;
  case SO::kDecrementWrap:
    return RS::DECREMENT_AND_WRAP;
  case SO::kKeep:
  default:
    return RS::KEEP;
  }
}

plume::RenderComparisonFunction
ConvertCompareFunc(rex::graphics::xenos::CompareFunction f) {
  using CF = rex::graphics::xenos::CompareFunction;
  using RC = plume::RenderComparisonFunction;
  switch (f) {
  case CF::kNever:
    return RC::NEVER;
  case CF::kLess:
    return RC::LESS;
  case CF::kEqual:
    return RC::EQUAL;
  case CF::kLessEqual:
    return RC::LESS_EQUAL;
  case CF::kGreater:
    return RC::GREATER;
  case CF::kNotEqual:
    return RC::NOT_EQUAL;
  case CF::kGreaterEqual:
    return RC::GREATER_EQUAL;
  case CF::kAlways:
  default:
    return RC::ALWAYS;
  }
}

plume::RenderComparisonFunction
ConvertDepthCompareFunc(rex::graphics::xenos::CompareFunction f,
                        bool reverse_z) {
  using RC = plume::RenderComparisonFunction;
  const RC direct = ConvertCompareFunc(f);
  if (!reverse_z)
    return direct;
  switch (direct) {
  case RC::LESS:
    return RC::GREATER;
  case RC::LESS_EQUAL:
    return RC::GREATER_EQUAL;
  case RC::GREATER:
    return RC::LESS;
  case RC::GREATER_EQUAL:
    return RC::LESS_EQUAL;
  default:
    return direct;
  }
}

plume::RenderBlend ConvertBlendMode(u32 factor) {
  switch (static_cast<rex::graphics::xenos::BlendFactor>(factor)) {
  case rex::graphics::xenos::BlendFactor::kZero:
    return plume::RenderBlend::ZERO;
  case rex::graphics::xenos::BlendFactor::kOne:
    return plume::RenderBlend::ONE;
  case rex::graphics::xenos::BlendFactor::kSrcColor:
    return plume::RenderBlend::SRC_COLOR;
  case rex::graphics::xenos::BlendFactor::kOneMinusSrcColor:
    return plume::RenderBlend::INV_SRC_COLOR;
  case rex::graphics::xenos::BlendFactor::kSrcAlpha:
    return plume::RenderBlend::SRC_ALPHA;
  case rex::graphics::xenos::BlendFactor::kOneMinusSrcAlpha:
    return plume::RenderBlend::INV_SRC_ALPHA;
  case rex::graphics::xenos::BlendFactor::kDstColor:
    return plume::RenderBlend::DEST_COLOR;
  case rex::graphics::xenos::BlendFactor::kOneMinusDstColor:
    return plume::RenderBlend::INV_DEST_COLOR;
  case rex::graphics::xenos::BlendFactor::kDstAlpha:
    return plume::RenderBlend::DEST_ALPHA;
  case rex::graphics::xenos::BlendFactor::kOneMinusDstAlpha:
    return plume::RenderBlend::INV_DEST_ALPHA;
  case rex::graphics::xenos::BlendFactor::kSrcAlphaSaturate:
    return plume::RenderBlend::SRC_ALPHA_SAT;
  default:
    return plume::RenderBlend::ONE;
  }
}

plume::RenderBlendOperation ConvertBlendOp(u32 op) {
  switch (op) {
  case 1:
    return plume::RenderBlendOperation::SUBTRACT;
  case 2:
    return plume::RenderBlendOperation::MIN;
  case 3:
    return plume::RenderBlendOperation::MAX;
  case 4:
    return plume::RenderBlendOperation::REV_SUBTRACT;
  default:
    return plume::RenderBlendOperation::ADD;
  }
}

}
