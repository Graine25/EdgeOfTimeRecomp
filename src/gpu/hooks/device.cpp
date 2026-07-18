#include <atomic>
#include <set>
#include <mutex>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/device/native_texture_mirror.h"
#include "gpu/device/host_resource_heap.h"
#include "gpu/guest/resources.h"

namespace eot::gpu {
void NoteFrameStartForDraws();
}

REX_EXTERN(__imp__D3DDevice_ClearF);
REX_HOOK_RAW(D3DDevice_ClearF) {
  const u32 flags = ctx.r4.u32;
  const u32 color_va = ctx.r6.u32;
  const float z = static_cast<float>(ctx.f1.f64);
  eot::gpu::Video::BeginGuestFrame();
  eot::gpu::NoteFrameStartForDraws();
  eot::gpu::Video::ClearBoundTargets(ctx.r3.u32, flags, color_va, z);
  __imp__D3DDevice_ClearF(ctx, base);
}

REX_HOOK_RAW(D3DDevice_Swap) {
  (void)base;
  auto *front_buffer = eot::gpu::ResolveGuestSurface(ctx.r4.u32);
  if (!front_buffer)
    front_buffer = eot::gpu::FindOrBuildNativeTexture(ctx.r4.u32);
  eot::gpu::Video::Present(front_buffer);
}

REX_EXTERN(__imp__D3DDevice_Resolve);
REX_HOOK_RAW(D3DDevice_Resolve) {
  const u32 flags = ctx.r4.u32;
  const u32 source_rect_va = ctx.r5.u32;
  const u32 dest_texture_va = ctx.r6.u32;
  const u32 dest_point_va = ctx.r7.u32;
  const u32 dest_level = ctx.r8.u32;

  eot::gpu::ResolveRegion region;
  if (source_rect_va) {
    region.left = static_cast<i32>(eot::mem::try_load<u32>(source_rect_va));
    region.top = static_cast<i32>(eot::mem::try_load<u32>(source_rect_va + 4));
    region.right = static_cast<i32>(eot::mem::try_load<u32>(source_rect_va + 8));
    region.bottom =
        static_cast<i32>(eot::mem::try_load<u32>(source_rect_va + 12));
    region.valid = region.right > region.left && region.bottom > region.top;
  }
  if (dest_point_va) {
    region.destX = static_cast<i32>(eot::mem::try_load<u32>(dest_point_va));
    region.destY = static_cast<i32>(eot::mem::try_load<u32>(dest_point_va + 4));
  }

  __imp__D3DDevice_Resolve(ctx, base);
  eot::gpu::Video::ResolveRenderTarget(flags, dest_texture_va, dest_level,
                                       region);
}
