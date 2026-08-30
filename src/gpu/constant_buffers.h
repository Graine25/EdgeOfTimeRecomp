#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

constexpr u64 kTexturePlacementAlignment = 512;
constexpr u64 kTextureRowPitchAlignment = 256;
constexpr u64 kConstantBufferAlignment = 256;

struct UploadAlloc {
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  u64 size = 0;
  explicit operator bool() const { return buffer != nullptr; }
};

bool UploadRingInit();
void UploadRingResetFrame(u32 slot);

bool UploadAllocate(u64 size, u64 alignment, UploadAlloc *out);
bool UploadBytes(const void *src, u64 size, u64 alignment, UploadAlloc *out);

u64 UploadRingBytesThisFrame();

}
