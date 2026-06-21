#include <rex/hook.h>

#include "src/goliath_engine/gpu/renderer/video.h"

REX_STUB(eot_RenderThread_Kick);

REX_STUB(D3DDevice_BlockUntilIdle);

REX_HOOK_RAW(D3DDevice_Swap) {
  (void)ctx;
  (void)base;
  eot::gpu::VideoPresent();
}
