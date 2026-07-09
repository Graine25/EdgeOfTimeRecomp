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

}
