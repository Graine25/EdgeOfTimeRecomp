#include <cstdint>

#include <rex/hook.h>

#include "core/memory_helpers.h"
#include "gpu/upload_census.h"

REX_EXTERN(__imp__eot_TextureAsset_LoadInitialResidentDescriptor); // (texture r3, chunk r4, stream r5, r6)
REX_EXTERN(__imp__eot_RZTexture_RefreshData);                     // (texture r3, chunk r4, stream r5)
REX_EXTERN(__imp__eot_RZTexture_FreeResidentDescriptors);         // (texture r3)
REX_EXTERN(__imp__eot_RZTexture_UnloadDiscardableData);           // (texture r3)

namespace {

constexpr uint32_t kBuiltDescriptor = 0x50;
constexpr uint32_t kOtherDescriptors[2] = {0x54, 0x5C};
constexpr uint32_t kDescriptorHeader = 4;

uint32_t BuiltDescriptor(uint32_t texture) {
  return texture ? eot::mem::load<uint32_t>(texture + kBuiltDescriptor) : 0;
}

uint32_t Header(uint32_t descriptor) {
  return descriptor ? eot::mem::load<uint32_t>(descriptor + kDescriptorHeader) : 0;
}

}

REX_HOOK_RAW(eot_TextureAsset_LoadInitialResidentDescriptor) {
  const uint32_t texture = ctx.r3.u32;
  __imp__eot_TextureAsset_LoadInitialResidentDescriptor(ctx, base);
  eot::gpu::NoteTextureResident(Header(BuiltDescriptor(texture)));
}

REX_HOOK_RAW(eot_RZTexture_RefreshData) {
  const uint32_t texture = ctx.r3.u32;
  const uint32_t before = BuiltDescriptor(texture);
  __imp__eot_RZTexture_RefreshData(ctx, base);
  const uint32_t after = BuiltDescriptor(texture);
  if (after != before)
    eot::gpu::NoteTextureResident(Header(after));
}

REX_HOOK_RAW(eot_RZTexture_FreeResidentDescriptors) {
  const uint32_t texture = ctx.r3.u32;
  uint32_t headers[3] = {Header(BuiltDescriptor(texture)), 0, 0};
  for (int i = 0; i < 2; ++i)
    headers[i + 1] = texture ? Header(eot::mem::load<uint32_t>(texture + kOtherDescriptors[i])) : 0;
  __imp__eot_RZTexture_FreeResidentDescriptors(ctx, base);
  for (uint32_t header : headers)
    eot::gpu::NoteTextureReleased(header);
}

REX_HOOK_RAW(eot_RZTexture_UnloadDiscardableData) {
  const uint32_t header = Header(BuiltDescriptor(ctx.r3.u32));
  __imp__eot_RZTexture_UnloadDiscardableData(ctx, base);
  eot::gpu::NoteTextureReleased(header);
}
