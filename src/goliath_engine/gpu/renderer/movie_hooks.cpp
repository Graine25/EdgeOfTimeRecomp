#include <rex/hook.h>

#include <cstdint>

#include "src/goliath_engine/gpu/renderer/render_state.h"

REX_EXTERN(__imp__sub_82319638);
REX_HOOK_RAW(sub_82319638) {
  const uint32_t yuvBuffers = ctx.r4.u32;
  __imp__sub_82319638(ctx, base);
  if (ctx.r3.u32 == 1) eot::render::SubmitMovieFrame(base, yuvBuffers);
}
