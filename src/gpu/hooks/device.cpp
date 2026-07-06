#include <rex/hook.h>
#include <rex/types.h>

#include "gpu/device/device.h"
#include "gpu/device/host_resource_heap.h"
#include "gpu/guest/resources.h"

REX_EXTERN(__imp__D3DDevice_ClearF);
REX_HOOK_RAW(D3DDevice_ClearF) {
  eot::gpu::Video::BeginGuestFrame();
  __imp__D3DDevice_ClearF(ctx, base);
}

REX_HOOK_RAW(D3DDevice_Swap) {
  (void)base;
  auto *front_buffer =
      eot::gpu::HostResourceHeap::FromGuest<eot::gpu::GuestTexture>(
          ctx.r4.u32);
  eot::gpu::Video::Present(front_buffer);
}
