#pragma once

#include <cstdint>

namespace eot::render {

inline constexpr uint32_t kVsConstOffset = 0x780;
inline constexpr uint32_t kVsConstBytes = 256 * 16;
inline constexpr uint32_t kPsConstOffset = 0x1780;
inline constexpr uint32_t kPsConstBytes = 224 * 16;
inline constexpr uint32_t kSharedBytes = 912;
inline constexpr uint32_t kDeclHandleOffset = 12216;
inline constexpr uint32_t kColorControlOffset = 0x2934 + 0x8;

inline constexpr float kGameWidth = 1280.0f;
inline constexpr float kGameHeight = 720.0f;

enum GuestPrimitiveType : uint32_t {
  D3DPT_POINTLIST = 1,
  D3DPT_LINELIST = 2,
  D3DPT_LINESTRIP = 3,
  D3DPT_TRIANGLELIST = 4,
  D3DPT_TRIANGLEFAN = 5,
  D3DPT_TRIANGLESTRIP = 6,
  D3DPT_QUADLIST = 13,
};

enum GuestDeclUsage : uint8_t {
  D3DDECLUSAGE_POSITION = 0,
  D3DDECLUSAGE_BLENDWEIGHT = 1,
  D3DDECLUSAGE_BLENDINDICES = 2,
  D3DDECLUSAGE_NORMAL = 3,
  D3DDECLUSAGE_PSIZE = 4,
  D3DDECLUSAGE_TEXCOORD = 5,
  D3DDECLUSAGE_TANGENT = 6,
  D3DDECLUSAGE_BINORMAL = 7,
  D3DDECLUSAGE_TESSFACTOR = 8,
  D3DDECLUSAGE_POSITIONT = 9,
  D3DDECLUSAGE_COLOR = 10,
  D3DDECLUSAGE_FOG = 11,
  D3DDECLUSAGE_DEPTH = 12,
  D3DDECLUSAGE_SAMPLE = 13,
};

enum GuestDeclType : uint32_t {
  D3DDECLTYPE_FLOAT1 = 0x2C83A4,
  D3DDECLTYPE_FLOAT2 = 0x2C23A5,
  D3DDECLTYPE_FLOAT3 = 0x2A23B9,
  D3DDECLTYPE_FLOAT4 = 0x1A23A6,
  D3DDECLTYPE_D3DCOLOR = 0x182886,
};

}
