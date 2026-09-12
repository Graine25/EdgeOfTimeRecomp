#include <rex/hook.h>

#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/trace.h"

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
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
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
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
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
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
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
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
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
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetVertexShader(ctx, base);
  }
  EOT_TRACE_CALL("SetVertexShader {:#x}", shader);
  trace::Bump(trace::Counter::SetVertexShader);
}

extern "C" REX_FUNC(D3DDevice_SetPixelShader) {
  FlushPendingUpDraw();
  const u32 shader = ctx.r4.u32;
  {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetPixelShader(ctx, base);
  }
  EOT_TRACE_CALL("SetPixelShader {:#x}", shader);
  trace::Bump(trace::Counter::SetPixelShader);
}

extern "C" REX_FUNC(D3DDevice_SetStreamSource) {
  FlushPendingUpDraw();
  const u32 stream = ctx.r4.u32, vb = ctx.r5.u32, offset = ctx.r6.u32, stride = ctx.r7.u32;
  {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetStreamSource(ctx, base);
  }
  EOT_TRACE_CALL("SetStreamSource {} vb={:#x} offset={} stride={}", stream, vb, offset, stride);
  trace::Bump(trace::Counter::SetStreamSource);
}

extern "C" REX_FUNC(D3DDevice_SetIndices) {
  FlushPendingUpDraw();
  const u32 ib = ctx.r4.u32;
  {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetIndices(ctx, base);
  }
  EOT_TRACE_CALL("SetIndices {:#x}", ib);
  trace::Bump(trace::Counter::SetIndices);
}
