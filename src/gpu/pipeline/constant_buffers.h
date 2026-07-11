#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu::constants {

struct SharedConstants {
  u32 texture2DIndices[16]{};   // c0..c3,   bytes 0..63
  u32 texture3DIndices[16]{};   // c4..c7,   bytes 64..127
  u32 textureCubeIndices[16]{}; // c8..c11,  bytes 128..191
  u32 texture1DIndices[16]{};   // c12..c15, bytes 192..255
  u32 samplerIndices[16]{};     // c16..c19, bytes 256..319

  u32 booleansArr[8]{}; // c20..c21, bytes 320..351

  u32 swappedTexcoords{};   // c22.x,  byte 352
  float halfPixelOffset[2]{}; // c22.yz, bytes 356..363
  float alphaThreshold{};   // c22.w,  byte 364
  u32 swappedNormals{};     // c23.x,  byte 368
  u32 swappedBinormals{};   // c23.y,  byte 372
  u32 swappedTangents{};    // c23.z,  byte 376
  u32 swappedBlendWeights{}; // c23.w, byte 380
  u32 swappedPositions{};   // c24.x,  byte 384
  u32 sintTexcoords{};      // c24.y,  byte 388
  u32 _pad_c24_zw[2]{};     // c24.zw, bytes 392..399

  u32 loopConstants[32][4]{}; // c25..c56, bytes 400..911
};
static_assert(sizeof(SharedConstants) == 912,
              "SharedConstants must match the recompiler's packoffset layout");

struct Allocation {
  u8 *memory = nullptr;
  plume::RenderBufferReference ref;
  u32 size = 0;

  bool valid() const { return memory != nullptr; }
};

struct DrawConstants {
  Allocation vs;
  Allocation ps;
  Allocation shared;

  bool valid() const { return vs.valid() && ps.valid() && shared.valid(); }
};

Allocation Allocate(u32 size);

bool CopyGuestSwapped32(u8 *dst, u32 guest_va, u32 bytes);

DrawConstants UploadDrawConstants(u32 device_va);

void ResetFrame(u32 slot);

void Shutdown();

void LogStats();

}
