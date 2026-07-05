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
  be_u32 DepthInfo;     // +0x1C  GPU_DEPTHINFO union word
  be_u32 HiControl;     // +0x20
  be_u32 SizeBits;      // +0x24  packed Height/Width
  be_u32 Format;        // +0x28  D3DFORMAT
  be_u32 Size;          // +0x2C
};
static_assert(sizeof(D3DSurface) == 48);

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

inline void InitResourceHeader(D3DResource &r, D3DResourceType type) {
  r.Common = u32(type);
  r.ReferenceCount = 1u;
  r.Fence = 0u;
  r.ReadFence = 0u;
  r.Identifier = 0u;
  r.BaseFlush = 0u;
}

}
