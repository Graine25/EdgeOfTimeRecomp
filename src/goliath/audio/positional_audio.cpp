#include <rex/cvar.h>
#include <rex/hook.h>

REX_EXTERN(__imp__eot_GLAPIAudio_IsCueResource3D); // (this r3, cue r4) -> bool

REX_HOOK_RAW(eot_GLAPIAudio_IsCueResource3D) {
  __imp__eot_GLAPIAudio_IsCueResource3D(ctx, base);
  if (!rex::cvar::Query<bool>("eot_spatial_audio"))
    ctx.r3.u64 = 0;
}
