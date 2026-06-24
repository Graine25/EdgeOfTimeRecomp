#include <rex/hook.h>

#include "src/goliath_engine/gpu/renderer/video.h"

REX_EXTERN(__imp__eot_RenderThread_Kick);
REX_HOOK_RAW(eot_RenderThread_Kick) {
  __imp__eot_RenderThread_Kick(ctx, base);
}

REX_HOOK_RAW(sub_82231448) {
  (void)ctx;
  (void)base;
}

REX_HOOK_RAW(sub_82230078) {
  (void)ctx;
  (void)base;
}

REX_STUB(D3DDevice_BlockUntilIdle);

REX_HOOK_RAW(D3DDevice_Swap) {
  (void)ctx;
  (void)base;
  eot::gpu::VideoPresent();
}
