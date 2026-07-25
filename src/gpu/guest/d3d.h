/**
 * @file    gpu/guest/d3d.h
 * @brief   Byte-accurate guest layouts for the guest D3D9 SDK resource
 *          headers the recompiled engine reads/writes.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * D3DResource/D3DTexture/D3DSurface mirror the standard Xbox 360 D3D9 XDK
 * struct layout, which is fixed by the SDK header every title compiles
 * against - so these are portable as-is. re:Blue's version of this file also
 * has a D3DDevice struct with "BD-private slots past the public 0x2A00
 * struct" (SetPixelShader/viewport/etc. write offsets) - those are re:Blue's
 * own reversing of where THEIR compiled hooks happened to land, not XDK ABI,
 * and are deliberately NOT ported here. EOT will need its own D3DDevice
 * reversing once device-state hooks (SetTexture, SetRenderTarget, ...) are in
 * scope.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

namespace eot::gpu {

enum class D3DResourceType : u32 {
  kSurface = 1,
  kTexture = 3,
  kVolumeTexture = 4,
  kCubeTexture = 5,
  kVertexBuffer = 6,
  kIndexBuffer = 7,
};

struct D3DResource {
  be_u32 Common;
  be_u32 ReferenceCount; // +0x04  engine increments/decrements directly
  be_u32 Fence;          // +0x08
  be_u32 ReadFence;      // +0x0C
  be_u32 Identifier;     // +0x10
  be_u32 BaseFlush;      // +0x14
};
static_assert(sizeof(D3DResource) == 24);

struct GpuTextureFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(GpuTextureFetchConstant) == 24);

struct D3DTexture {
  D3DResource resource;           // +0x00
  be_u32 MipFlush;                // +0x18
  GpuTextureFetchConstant Format; // +0x1C
};
static_assert(sizeof(D3DTexture) == 52);
static_assert(offsetof(D3DTexture, Format) == 0x1C);

struct D3DSurface {
  D3DResource resource; // +0x00
  be_u32 SurfaceInfo;   // +0x18  GPU_SURFACEINFO union word
  be_u32 DepthInfo;
  be_u32 HiControl;     // +0x20
  be_u32 SizeBits;      // +0x24  packed Height/Width
  be_u32 Format;        // +0x28  D3DFORMAT
  be_u32 Size;          // +0x2C
};
static_assert(sizeof(D3DSurface) == 48);

inline u32 SurfaceWidthFromSizeBits(u32 size_bits) {
  return (size_bits >> 18) + 1u;
}
inline u32 SurfaceHeightFromSizeBits(u32 size_bits) {
  return ((size_bits >> 3) & 0x7FFFu) + 1u;
}

struct D3DSurfaceDesc {
  be_u32 Format;             // +0x00  guest D3DFORMAT
  be_u32 Type;               // +0x04  D3DRESOURCETYPE
  be_u32 Usage;              // +0x08
  be_u32 Pool;               // +0x0C
  be_u32 MultiSampleType;    // +0x10
  be_u32 MultiSampleQuality; // +0x14
  be_u32 Width;              // +0x18  surface / mip-level width
  be_u32 Height;             // +0x1C  surface / mip-level height
};
static_assert(sizeof(D3DSurfaceDesc) == 32);

struct D3DLockedRect {
  be_u32 Pitch; // +0x00  row pitch in bytes
  be_u32 pBits; // +0x04  guest VA of the locked pixel scratch
};
static_assert(sizeof(D3DLockedRect) == 8);

inline constexpr u32 kDeviceRenderTargetShadow = 0x31F8;
inline constexpr u32 kDeviceDepthStencilShadow = 0x3208;

inline constexpr u32 kMaxRenderTargets = 4;

inline constexpr u32 kDeviceIndexBufferShadow = 0x31F4;
inline constexpr u32 kDeviceStreamSourceShadow = 0x320C;

inline constexpr u32 kMaxStreamSources = 16;

inline constexpr u32 kStreamFetchDword0 = 0x778;
inline constexpr u32 kStreamFetchStride = 8;

inline constexpr u32 kIndexBuffer32BitFlag = 0x80000000;

struct D3DVertexBuffer {
  D3DResource Resource; // +0x00
  be_u32 FetchAddress; // +0x18
  be_u32 FetchSize;    // +0x1C
};
static_assert(sizeof(D3DVertexBuffer) == 32);

struct D3DIndexBuffer {
  D3DResource Resource; // +0x00  format lives in Common >> 29
  be_u32 Address;       // +0x18  guest VA of the index data
  be_u32 Size;          // +0x1C  bytes
};
static_assert(sizeof(D3DIndexBuffer) == 32);

inline constexpr u32 kTextureObjectShadow = 0x3260;

inline constexpr u32 kTextureObjectFetchOffset = 0x1C;

inline constexpr u32 kTextureFetchConstants = 0x480;
inline constexpr u32 kTextureFetchStride = 24;

inline constexpr u32 kViewportOffset = 0x32C8;

inline constexpr u32 kDestColor0InfoOffset = 0x2884;
inline constexpr u32 kDestDepthInfoOffset = 0x2888;

inline constexpr u32 DestColorInfoOffset(u32 index) {
  return index == 0 ? kDestColor0InfoOffset
                    : kDestDepthInfoOffset + index * 4u;
}

inline constexpr u32 kWindowScissorTLOffset = 0x28C4;
inline constexpr u32 kWindowScissorBROffset = 0x28C8;

inline constexpr u32 ScissorX(u32 packed) { return packed & 0x7FFFu; }
inline constexpr u32 ScissorY(u32 packed) { return (packed >> 16) & 0x7FFFu; }

inline constexpr u32 kViewportZScaleOffset = 0x2918;
inline constexpr u32 kViewportZOffsetOffset = 0x291C;

inline constexpr u32 kStencilRefMaskOffset = 0x2900;

inline constexpr u32 kPolyOffsetFrontScaleOffset = 0x2A50;
inline constexpr u32 kPolyOffsetFrontOffset = 0x2A54;
inline constexpr u32 kPolyOffsetBackScaleOffset = 0x2A58;
inline constexpr u32 kPolyOffsetBackOffset = 0x2A5C;

inline constexpr u32 kModeControlOffset = 0x2948;

inline constexpr u32 kColorMaskOffset = 0x28DC;

inline constexpr u32 kDepthControlOffset = 0x2934;
inline constexpr u32 kBlendControl0Offset = 0x2938;
inline constexpr u32 kColorControlOffset = 0x293C;

inline constexpr u32 kAlphaRefOffset = 0x2904;

inline constexpr u32 kMaxSamplerSlots = 16;

inline constexpr u32 kVsFloatConstOffset = 0x780;
inline constexpr u32 kPsFloatConstOffset = 0x1780;
inline constexpr u32 kVsFloatConstCount = 256;
inline constexpr u32 kPsFloatConstCount = 224;
inline constexpr u32 kVsFloatConstBytes = kVsFloatConstCount * 16;
inline constexpr u32 kPsFloatConstBytes = kPsFloatConstCount * 16;

inline constexpr u32 kVsBoolConstOffset = 0x2780;
inline constexpr u32 kPsBoolConstOffset = 0x2790;
inline constexpr u32 kBoolConstDwords = 4;

inline constexpr u32 kLoopConstOffset = 0x27A0;
inline constexpr u32 kLoopConstCount = 32;

inline constexpr u32 kDeviceVertexDeclShadow = 0x2FB8;

inline constexpr u32 kVertexDeclCommonSignature = 0x100005;
inline constexpr u32 kVertexDeclBaseFlushSignature = 0xFFFF0000;

inline constexpr u32 kVertexDeclEndStream = 0xFF;

inline constexpr u32 kVertexDeclCountOffset = 0x18;
inline constexpr u32 kVertexDeclMaxStreamOffset = 0x1C;
inline constexpr u32 kVertexDeclElementsOffset = 0x34;
inline constexpr u32 kVertexDeclElementStride = 12;

struct D3DVertexElement {
  be_u32 StreamAndOffset;
  be_u32 Type;            // D3DDECLTYPE
  be_u32 MethodUsage;
};
static_assert(sizeof(D3DVertexElement) == 12);

inline u32 VertexElementStream(const D3DVertexElement &e) {
  return u32(e.StreamAndOffset) >> 16;
}
inline u32 VertexElementOffset(const D3DVertexElement &e) {
  return u32(e.StreamAndOffset) & 0xFFFF;
}
inline u32 VertexElementUsage(const D3DVertexElement &e) {
  return (u32(e.MethodUsage) >> 16) & 0xFF;
}
inline u32 VertexElementUsageIndex(const D3DVertexElement &e) {
  return (u32(e.MethodUsage) >> 8) & 0xFF;
}

inline constexpr u32 kVertexFetchTypeMask = 0x3;
inline constexpr u32 kVertexFetchSizeMask = 0x3FFFFFC;

inline u32 VertexBufferAddress(const D3DVertexBuffer &vb) {
  return u32(vb.FetchAddress) & ~kVertexFetchTypeMask;
}
inline u32 VertexBufferSize(const D3DVertexBuffer &vb) {
  return u32(vb.FetchSize) & kVertexFetchSizeMask;
}
inline u32 IndexBufferFormat(const D3DIndexBuffer &ib) {
  return u32(ib.Resource.Common) >> 29;
}
inline bool IndexBufferIs32Bit(const D3DIndexBuffer &ib) {
  return (u32(ib.Resource.Common) & kIndexBuffer32BitFlag) != 0;
}

inline constexpr u32 kDevicePixelShaderShadow = 0x32F4;
inline constexpr u32 kDeviceVertexShaderShadow = 0x32F8;

struct ShaderContainer {
  be_u32 Flags;                 // +0x00  0x102A11'00 pixel / '01 vertex
  be_u32 VirtualSize;           // +0x04  header + microcode, i.e. this blob
  be_u32 PhysicalSize;          // +0x08  trailing XG registration payload
  be_u32 Field0C;               // +0x0C
  be_u32 ConstantTableOffset;   // +0x10
  be_u32 DefinitionTableOffset; // +0x14
  be_u32 ShaderOffset;          // +0x18
  be_u32 Field1C;               // +0x1C  zero in every valid container
  be_u32 Field20;               // +0x20  zero in every valid container
};
static_assert(sizeof(ShaderContainer) == 36);

inline constexpr u32 kShaderContainerMagic = 0x102A1100;
inline constexpr u32 kShaderContainerMagicMask = 0xFFFFFF00;
inline constexpr u32 kShaderContainerVertexBit = 1;

inline constexpr u32 kPixelShaderContainerOffset = 0x28;
inline constexpr u32 kVertexShaderContainerOffset = 0x368;

inline constexpr u32 kShaderNodePhysicalPayload = 0x08;
inline constexpr u32 kShaderNodeVertexObject = 0x24;
inline constexpr u32 kShaderNodePixelObject = 0x20;

inline void InitResourceHeader(D3DResource &r, D3DResourceType type) {
  r.Common = u32(type);
  r.ReferenceCount = 1u;
  r.Fence = 0u;
  r.ReadFence = 0u;
  r.Identifier = 0u;
  r.BaseFlush = 0u;
}

}
