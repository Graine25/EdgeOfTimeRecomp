#include <cstring>

#include <rex/hook.h>

#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/hooks/fast_guest.h"
#include "gpu/settings.h"
#include "gpu/trace.h"

using namespace eot::gpu;

REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice_ClearF);
REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_BeginIndexedVertices);
REX_EXTERN(__imp__sub_8223A738);
REX_EXTERN(__imp__sub_82232178);

namespace {

using namespace eot::gpu::fastguest;

void FastDrawFlush(PPCContext &ctx, u8 *base, u32 device) {
  u8 *dev = Guest(base, device);
  const u64 p0 = Ld64(dev), p1 = Ld64(dev + 8), p2 = Ld64(dev + 16), p3 = Ld64(dev + 24),
            p4 = Ld64(dev + 32);
  if (p0)
    St64(dev, 0);
  if (p1)
    St64(dev + 8, 0);
  if (p2) {
    if (p2 & 0x1E0000ull) {
      ctx.r3.u64 = device;
      ctx.r4.u64 = p2;
      __imp__sub_8223A738(ctx, base);
    }
    St64(dev + 16, 0);
  }
  if (p3)
    St64(dev + 24, 0);
  if (p4) {
    if ((p4 & 0xC000000000000000ull) && (dev[11072] & 0xC0)) {
      ctx.r3.u64 = device;
      __imp__sub_82232178(ctx, base);
    }
    St64(dev + 32, 0);
  }
}

constexpr DeviceCompare::Range kDrawSkip[] = {{40, 64}, {11064, 11072}, {13600, 13624}};

template <typename Original>
void DrawFlush(PPCContext &ctx, u8 *base, u32 device, const char *what, Original original) {
  static const bool verify = Settings::FastSettersVerify();
  if (device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      DeviceCompare cmp;
      cmp.Snapshot(dev, cmp.before);
      const PPCContext saved = ctx;
      FastDrawFlush(ctx, base, device);
      cmp.Snapshot(dev, cmp.after_fast);
      cmp.Restore(dev);
      ctx = saved;
      original();
      cmp.Snapshot(dev, cmp.after_orig);
      cmp.Report(what, kDrawSkip, 3);
    } else {
      FastDrawFlush(ctx, base, device);
    }
    return;
  }
  PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
  original();
}

u64 ReverseBits(u64 v) {
  v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
  v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
  v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
  return __builtin_bswap64(v);
}

FloatConstantDirty PendingFloatConstants(u32 device_va) {
  if (!device_va)
    return {};
  const u8 *pending = eot::mem::at<u8>(device_va + eot::gpu::dev::kPendingMask);
  if (!pending)
    return {};
  return {ReverseBits(Ld64(pending)), ReverseBits(Ld64(pending + 8))};
}

}

extern "C" REX_FUNC(D3DDevice_DrawVertices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  const FloatConstantDirty constants = PendingFloatConstants(device);
  DrawFlush(ctx, base, device, "DrawVertices",
            [&] { __imp__D3DDevice_DrawVertices(ctx, base); });
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
  PrefetchIndexProbes(device, start_index, count);
  DrawFlush(ctx, base, device, "DrawIndexedVertices",
            [&] { __imp__D3DDevice_DrawIndexedVertices(ctx, base); });
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
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
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
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
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
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
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
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
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
