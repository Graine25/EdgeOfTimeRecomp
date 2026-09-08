#include <rex/hook.h>
#include <cmath>
#include <cstdint>
#include <mutex>

#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/trace.h"
#include "core/logging.h"

using namespace eot;
using namespace eot::gpu;

REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
REX_EXTERN(__imp__D3DDevice_SetDepthStencilSurface);
REX_EXTERN(__imp__D3DDevice_SetViewport);
REX_EXTERN(__imp__D3DDevice_SetTexture);
REX_EXTERN(__imp__D3DDevice_SetVertexShader);
REX_EXTERN(__imp__D3DDevice_SetPixelShader);
REX_EXTERN(__imp__D3DDevice_SetStreamSource);
REX_EXTERN(__imp__D3DDevice_SetIndices);

extern "C" REX_FUNC(D3DDevice_SetRenderTarget) {
  FlushPendingUpDraw();
  const u32 index = ctx.r4.u32, surface = ctx.r5.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_SetRenderTarget(ctx, base);
  }
  if (trace::Enabled()) {
    const u32 info = surface ? mem::load<u32>(surface + 0x1C) : 0;
    const u32 size = surface ? mem::load<u32>(surface + 0x24) : 0;
    EOT_TRACE_CALL("SetRenderTarget {} surf={:#x} {}x{} fmt={} tile={}", index, surface,
                   surface ? (size >> 18) + 1 : 0, surface ? ((size >> 3) & 0x7FFF) + 1 : 0,
                   (info >> 16) & 0xF, info & 0xFFF);
  }
  trace::Bump(trace::Counter::SetRenderTarget);
}

extern "C" REX_FUNC(D3DDevice_SetDepthStencilSurface) {
  FlushPendingUpDraw();
  const u32 surface = ctx.r4.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_SetDepthStencilSurface(ctx, base);
  }
  if (trace::Enabled()) {
    const u32 size = surface ? mem::load<u32>(surface + 0x24) : 0;
    EOT_TRACE_CALL("SetDepthStencilSurface surf={:#x} {}x{}", surface,
                   surface ? (size >> 18) + 1 : 0, surface ? ((size >> 3) & 0x7FFF) + 1 : 0);
  }
  trace::Bump(trace::Counter::SetDepth);
}

extern "C" REX_FUNC(D3DDevice_SetViewport) {
  FlushPendingUpDraw();
  const u32 vp = ctx.r4.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_SetViewport(ctx, base);
  }
  if (trace::Enabled() && vp) {
    EOT_TRACE_CALL("SetViewport x={} y={} w={} h={} minZ={} maxZ={}", mem::u32at(vp),
                   mem::u32at(vp + 4), mem::u32at(vp + 8), mem::u32at(vp + 12),
                   mem::f32at(vp + 16), mem::f32at(vp + 20));
  }
  trace::Bump(trace::Counter::SetViewport);
}

extern "C" REX_FUNC(D3DDevice_SetTexture) {
  FlushPendingUpDraw();
  const u32 sampler = ctx.r4.u32, texture = ctx.r5.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_SetTexture(ctx, base);
  }
  if (trace::Enabled()) {
    const u32 d1 = texture ? mem::load<u32>(texture + 0x1C + 4) : 0;
    const u32 d2 = texture ? mem::load<u32>(texture + 0x1C + 8) : 0;
    EOT_TRACE_CALL("SetTexture {} tex={:#x} fmt={} {}x{}", sampler, texture, d1 & 0x3F,
                   (d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1);
  }
  trace::Bump(trace::Counter::SetTexture);
}

extern "C" REX_FUNC(D3DDevice_SetVertexShader) {
  FlushPendingUpDraw();
  const u32 shader = ctx.r4.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_SetVertexShader(ctx, base);
  }
  EOT_TRACE_CALL("SetVertexShader {:#x}", shader);
  trace::Bump(trace::Counter::SetVertexShader);
}

extern "C" REX_FUNC(D3DDevice_SetPixelShader) {
  FlushPendingUpDraw();
  const u32 shader = ctx.r4.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_SetPixelShader(ctx, base);
  }
  EOT_TRACE_CALL("SetPixelShader {:#x}", shader);
  trace::Bump(trace::Counter::SetPixelShader);
}

extern "C" REX_FUNC(D3DDevice_SetStreamSource) {
  FlushPendingUpDraw();
  const u32 stream = ctx.r4.u32, vb = ctx.r5.u32, offset = ctx.r6.u32, stride = ctx.r7.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_SetStreamSource(ctx, base);
  }
  EOT_TRACE_CALL("SetStreamSource {} vb={:#x} offset={} stride={}", stream, vb, offset, stride);
  trace::Bump(trace::Counter::SetStreamSource);
}

extern "C" REX_FUNC(D3DDevice_SetIndices) {
  FlushPendingUpDraw();
  const u32 ib = ctx.r4.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_SetIndices(ctx, base);
  }
  EOT_TRACE_CALL("SetIndices {:#x}", ib);
  trace::Bump(trace::Counter::SetIndices);
}

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

using namespace eot::gpu;

REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice_ClearF);
REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_BeginIndexedVertices);

extern "C" REX_FUNC(D3DDevice_DrawVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_DrawVertices(ctx, base);
  }
  EOT_TRACE_CALL("DrawVertices prim={} start={} count={}", prim, start, count);
  trace::Bump(trace::Counter::DrawVertices);
  DrawGuestPrimitives(device, prim, start, count);
}

extern "C" REX_FUNC(D3DDevice_DrawIndexedVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, prim = ctx.r4.u32;
  const i32 base_vertex = ctx.r5.s32;
  const u32 start_index = ctx.r6.u32, count = ctx.r7.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_DrawIndexedVertices(ctx, base);
  }
  EOT_TRACE_CALL("DrawIndexedVertices prim={} base={} start={} count={}", prim, base_vertex,
                 start_index, count);
  trace::Bump(trace::Counter::DrawIndexed);
  DrawGuestIndexedPrimitives(device, prim, base_vertex, start_index, count);
}

extern "C" REX_FUNC(D3DDevice_ClearF) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, color = ctx.r6.u32;
  const float z = static_cast<float>(ctx.f1.f64);
  const u32 stencil = ctx.r7.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_ClearF(ctx, base);
  }
  EOT_TRACE_CALL("ClearF flags={:#x} rect={:#x} color={:#x} z={} stencil={}", flags, rect, color,
                 z, stencil);
  trace::Bump(trace::Counter::Clear);
  ClearGuestTargets(device, flags, rect, color, z, stencil);
}

extern "C" REX_FUNC(D3DDevice_Resolve) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, flags = ctx.r4.u32, src_rect = ctx.r5.u32, dest = ctx.r6.u32;
  const u32 dest_point = ctx.r7.u32, dest_level = ctx.r8.u32, clear_color = ctx.r9.u32;
  const u32 r10 = ctx.r10.u32;
  const float clear_z = static_cast<float>(ctx.f1.f64);
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_Resolve(ctx, base);
  }
  EOT_TRACE_CALL("Resolve flags={:#x} src={} rect={:#x} dest={:#x} point={:#x} level={} "
                 "r9={:#x} r10={:#x} z={}",
                 flags, flags & 7, src_rect, dest, dest_point, dest_level, clear_color, r10,
                 clear_z);
  trace::Bump(trace::Counter::Resolve);
  ResolveGuest(device, flags, src_rect, dest, dest_point, dest_level, clear_color, clear_z);
}

extern "C" REX_FUNC(D3DDevice_BeginVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_BeginVertices(ctx, base);
  }
  const u32 data = ctx.r3.u32;
  EOT_TRACE_CALL("BeginVertices prim={} count={} stride={} data={:#x}", prim, count, stride, data);
  trace::Bump(trace::Counter::BeginVertices);
  QueueGuestUpDraw(device, prim, count, stride, data);
}

extern "C" REX_FUNC(D3DDevice_BeginIndexedVertices) {
  FlushPendingUpDraw();
  const u32 prim = ctx.r4.u32;
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_BeginIndexedVertices(ctx, base);
  }
  EOT_TRACE_CALL("BeginIndexedVertices prim={} (unmodelled)", prim);
  trace::Bump(trace::Counter::BeginVertices);
}
