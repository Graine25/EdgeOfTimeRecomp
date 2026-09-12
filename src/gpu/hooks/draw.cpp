#include <rex/hook.h>

#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/trace.h"

using namespace eot::gpu;

REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice_ClearF);
REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_BeginIndexedVertices);

namespace {

FloatConstantDirty PendingFloatConstants(u32 device_va) {
  if (!device_va)
    return {};
  const DeviceView dev = Device(device_va);
  auto group_dirty = [&](u32 group) {
    const u32 off = eot::gpu::dev::kPendingMask + group * 8;
    return dev.U32(off) != 0 || dev.U32(off + 4) != 0;
  };
  return {group_dirty(0), group_dirty(1)};
}

}

extern "C" REX_FUNC(D3DDevice_DrawVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  const FloatConstantDirty constants = PendingFloatConstants(device);
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_DrawVertices(ctx, base);
  }
  EOT_TRACE_CALL("DrawVertices prim={} start={} count={}", prim, start, count);
  trace::Bump(trace::Counter::DrawVertices);
  DrawGuestPrimitives(device, prim, start, count, constants);
}

extern "C" REX_FUNC(D3DDevice_DrawIndexedVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, prim = ctx.r4.u32;
  const i32 base_vertex = ctx.r5.s32;
  const u32 start_index = ctx.r6.u32, count = ctx.r7.u32;
  const FloatConstantDirty constants = PendingFloatConstants(device);
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_DrawIndexedVertices(ctx, base);
  }
  EOT_TRACE_CALL("DrawIndexedVertices prim={} base={} start={} count={}", prim, base_vertex,
                 start_index, count);
  trace::Bump(trace::Counter::DrawIndexed);
  DrawGuestIndexedPrimitives(device, prim, base_vertex, start_index, count, constants);
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
  const FloatConstantDirty constants = PendingFloatConstants(device);
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_BeginVertices(ctx, base);
  }
  const u32 data = ctx.r3.u32;
  EOT_TRACE_CALL("BeginVertices prim={} count={} stride={} data={:#x}", prim, count, stride, data);
  trace::Bump(trace::Counter::BeginVertices);
  QueueGuestUpDraw(device, prim, count, stride, data, constants);
}

extern "C" REX_FUNC(D3DDevice_BeginIndexedVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32;
  const u32 prim = ctx.r4.u32;
  const FloatConstantDirty constants = PendingFloatConstants(device);
  {
    PerfScope guest_scope(state().perf.guest_d3d_ms);
    state().perf.guest_d3d_calls++;
    __imp__D3DDevice_BeginIndexedVertices(ctx, base);
  }
  if (constants.vs || constants.ps) {
    auto &s = state();
    std::lock_guard lock(s.guest_mutex);
    s.vs_float_constants_stale |= constants.vs;
    s.ps_float_constants_stale |= constants.ps;
  }
  EOT_TRACE_CALL("BeginIndexedVertices prim={} (unmodelled)", prim);
  trace::Bump(trace::Counter::BeginVertices);
}
