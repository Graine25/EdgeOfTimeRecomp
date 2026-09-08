#include <cmath>
#include <cstdint>
#include <mutex>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/trace.h"

using namespace eot;
using namespace eot::gpu;

REX_EXTERN(__imp__D3DDevice_SetGammaRamp);
REX_EXTERN(__imp__D3DDevice_SetPWLGamma);

extern "C" REX_FUNC(D3DDevice_Swap) {
  FlushPendingUpDraw();
  const u32 front = ctx.r4.u32;
  EOT_TRACE_CALL("Swap front={:#x}", front);
  Video::Present(front);
  ctx.r3.u64 = 0;
}

constexpr u32 kDeviceGammaShadow = 0x3C20;

extern "C" REX_FUNC(D3DDevice_SetGammaRamp) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32;
  const u32 ramp = ctx.r4.u32;
  __imp__D3DDevice_SetGammaRamp(ctx, base);
  EOT_TRACE_CALL("SetGammaRamp ramp={:#x}", ramp);
  if (!ramp || !device)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  for (u32 c = 0; c < 3; ++c)
    for (u32 i = 0; i < 256; ++i)
      s.gamma_table[c][i] =
          mem::load<uint16_t>(device + kDeviceGammaShadow + (c * 256 + i) * 2);
  s.gamma_mode = VideoState::GammaMode::Table;
  s.gamma_lut_dirty = true;
  EOT_DEBUG("[gamma] 256-entry ramp: r[0]={} r[32]={} r[64]={} r[128]={} r[255]={}",
           s.gamma_table[0][0], s.gamma_table[0][32], s.gamma_table[0][64], s.gamma_table[0][128],
           s.gamma_table[0][255]);
}

extern "C" REX_FUNC(D3DDevice_SetPWLGamma) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32;
  const u32 ramp = ctx.r4.u32;
  __imp__D3DDevice_SetPWLGamma(ctx, base);
  EOT_TRACE_CALL("SetPWLGamma ramp={:#x}", ramp);
  if (!ramp || !device)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  for (u32 c = 0; c < 3; ++c) {
    for (u32 i = 0; i < 128; ++i) {
      const u32 entry = device + kDeviceGammaShadow + (c * 128 + i) * 4;
      s.gamma_pwl[c][i][0] = mem::load<uint16_t>(entry);
      s.gamma_pwl[c][i][1] = mem::load<uint16_t>(entry + 2);
    }
  }
  s.gamma_mode = VideoState::GammaMode::Pwl;
  s.gamma_lut_dirty = true;
  EOT_DEBUG("[gamma] PWL ramp: r[0]={}+{} r[16]={}+{} r[32]={}+{} r[64]={}+{} r[127]={}+{}",
           s.gamma_pwl[0][0][0], s.gamma_pwl[0][0][1], s.gamma_pwl[0][16][0],
           s.gamma_pwl[0][16][1], s.gamma_pwl[0][32][0], s.gamma_pwl[0][32][1],
           s.gamma_pwl[0][64][0], s.gamma_pwl[0][64][1], s.gamma_pwl[0][127][0],
           s.gamma_pwl[0][127][1]);
}
