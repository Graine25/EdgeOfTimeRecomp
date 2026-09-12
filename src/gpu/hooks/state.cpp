#include <cstring>

#include <rex/hook.h>
#include <rex/memory/utils.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/hooks/fast_guest.h"
#include "gpu/settings.h"
#include "gpu/trace.h"

using namespace eot;
using namespace eot::gpu;

namespace {

using namespace eot::gpu::fastguest;

constexpr u32 kDevSamplerMinMip = 12332;
constexpr u32 kDevSamplerMaxMip = 12358;
constexpr u32 kDevDeclStride = 12240;
constexpr u32 kDevShaderFlags = 11070;   // 0x2B3E: bit 7 cleared by SetVertexShader
constexpr u32 kDevPendingGroup0 = 0;
constexpr u32 kDevPendingGroup1 = 8;
constexpr u32 kDevPendingGroup2 = 16;
constexpr u32 kDevPendingGroup3 = 24;
constexpr u32 kDevPendingGroup4 = 32;
constexpr u32 kDevConstantArea = 1152;
constexpr u32 kTexFetch = 28;
constexpr u32 kVbAddress = 24, kVbSize = 28;
constexpr u32 kPsLiteralTable = 60;
constexpr u32 kVsRecord = 872;

void ApplyLiteralTable(u8 *dev, const u8 *table, u32 pending_group) {
  St64(dev + pending_group, Ld64(dev + pending_group) & ~Ld64(table));
  if (Ld64(table + 8) != 0)
    St64(dev + kDevPendingGroup4, Ld64(dev + kDevPendingGroup4) | (1ull << 56));
  const u32 size = Ld32(table + 16);
  const u8 *p = table + 20;
  const u8 *end = p + size;
  while (p < end) {
    const u16 count = Ld16(p + 2);
    p += 4;
    if (count == 0)
      break;
    p += 4;
  }
  if (p >= end)
    return;
  while (p < end) {
    const u16 off = Ld16(p), count = Ld16(p + 2);
    p += 4;
    if (count == 0)
      break;
    std::memcpy(dev + kDevConstantArea + off, p, count * 4u);
    p += count * 4u;
  }
  while (p < end) {
    const u16 off = Ld16(p);
    u32 count = Ld16(p + 2);
    p += 4;
    if (count == 0)
      return;
    u8 *dst = dev + kDevConstantArea + off;
    do {
      const u32 mask = Ld32(p), value = Ld32(p + 4);
      St32(dst, (Ld32(dst) & mask) | value);
      dst += 4;
      p += 8;
      count = (count + 65536u - 2u) & 0xFFFFu;
    } while (count != 0);
  }
}

bool FastSetPixelShader(u8 *base, u32 device, u32 shader) {
  u8 *dev = Guest(base, device);
  const u32 old = Ld32(dev + dev::kPixelShader);
  if (NeedsRing(base, dev, old))
    return false;
  StampReplaced(base, dev, old);
  St32(dev + dev::kPixelShader, shader);
  St64(dev + kDevPendingGroup2, Ld64(dev + kDevPendingGroup2) | 0x120000ull);
  if (!shader)
    return true;
  const u8 *obj = Guest(base, shader);
  const u32 table = Ld32(obj + kPsLiteralTable);
  if (table)
    ApplyLiteralTable(dev, obj + 40 + table, kDevPendingGroup1);
  return true;
}

bool FastSetVertexShader(u8 *base, u32 device, u32 shader) {
  u8 *dev = Guest(base, device);
  const u32 old = Ld32(dev + dev::kVertexShader);
  if (NeedsRing(base, dev, old))
    return false;
  if (shader)
    St64(dev + kDevPendingGroup2, Ld64(dev + kDevPendingGroup2) | 0x80000ull);
  StampReplaced(base, dev, old);
  dev[kDevShaderFlags] &= 0x7F;
  St32(dev + dev::kVertexShader, shader);
  if (!shader || shader + kVsRecord == 0)
    return true;
  const u8 *rec = Guest(base, shader + kVsRecord);
  const u32 table = Ld32(rec + 20);
  if (table)
    ApplyLiteralTable(dev, rec + table, kDevPendingGroup0);
  return true;
}

bool FastSetTexture(u8 *base, u32 device, u32 sampler, u32 texture, u64 mask) {
  u8 *dev = Guest(base, device);
  const u32 old = Ld32(dev + dev::kTextureObject0 + 4 * sampler);
  if (NeedsRing(base, dev, old))
    return false;
  u8 *slot = dev + dev::kFetchConstants + 24 * sampler;
  if (texture) {
    const u8 *tex = Guest(base, texture) + kTexFetch;
    const u32 t0 = Ld32(tex), t1 = Ld32(tex + 4), t2 = Ld32(tex + 8), t3 = Ld32(tex + 12),
              t4 = Ld32(tex + 16), t5 = Ld32(tex + 20);
    const u32 d0 = Ld32(slot), d1 = Ld32(slot + 4), d3 = Ld32(slot + 12), d4 = Ld32(slot + 16),
              d5 = Ld32(slot + 20);
    const u32 base_addr = ((((t1 >> 20) & 0xFFFu) + 512u) & 0x1000u) + (t1 & 0x1FFFFFFFu);
    const u32 mip_addr = ((((t5 >> 20) & 0xFFFu) + 512u) & 0x1000u) + (t5 & 0x1FFFFE00u);
    u32 n4 = (d4 & ~0x3FCu) | (t4 & 0x3FCu);
    u32 lo = dev[kDevSamplerMinMip + sampler];
    if (const u32 tlo = (t4 >> 2) & 0xFu; tlo > lo)
      lo = tlo;
    n4 = (n4 & ~0x3Cu) | ((lo << 2) & 0x3Cu);
    u32 hi = dev[kDevSamplerMaxMip + sampler];
    if (const u32 thi = (t4 >> 6) & 0xFu; thi < hi)
      hi = thi;
    n4 = (n4 & ~0x3C0u) | ((hi << 6) & 0x3C0u);
    St32(slot, (d0 & 0x3FFC00u) | (t0 & ~0x3FFC00u));
    St32(slot + 4, (d1 & 0x800u) | (base_addr & ~0x800u));
    St32(slot + 8, t2);
    St32(slot + 12, (d3 & 0x7FF80000u) | (t3 & ~0x7FF80000u));
    St32(slot + 16, n4);
    St32(slot + 20, (d5 & 0x1FFu) | (mip_addr & ~0x1FFu));
    St64(dev + kDevPendingGroup3, Ld64(dev + kDevPendingGroup3) | mask);
  } else {
    St32(slot, Ld32(slot) & ~3u);
  }
  St32(dev + dev::kTextureObject0 + 4 * sampler, texture);
  StampReplaced(base, dev, old);
  return true;
}

bool FastSetStreamSource(u8 *base, u32 device, u32 stream, u32 vb, u32 offset, u32 stride,
                         u64 mask) {
  u8 *dev = Guest(base, device);
  const u32 old = Ld32(dev + dev::kStreamObject0 + 4 * stream);
  if (NeedsRing(base, dev, old))
    return false;
  if (vb) {
    const u8 *obj = Guest(base, vb);
    const u32 addr = Ld32(obj + kVbAddress) + offset;
    const u32 size = Ld32(obj + kVbSize);
    u8 *fetch = dev + dev::StreamFetchSlotOffset(stream);
    St32(fetch, ((((addr >> 20) & 0xFFFu) + 512u) & 0x1000u) + (addr & 0x1FFFFFFFu));
    St32(fetch + 4, size - offset);
    St64(dev + kDevPendingGroup3, Ld64(dev + kDevPendingGroup3) | mask);
  }
  StampReplaced(base, dev, old);
  St32(dev + dev::kStreamObject0 + 4 * stream, vb);
  const u32 stride4 = stride >> 2;
  dev[dev::kStreamStride0 + stream] = static_cast<u8>(stride4);
  const u32 s = stride4 & 0x3FFFFFFFu;
  if (s != 0 && s != dev[kDevDeclStride + stream])
    St64(dev + kDevPendingGroup2, Ld64(dev + kDevPendingGroup2) | 0x80000u);
  return true;
}

bool FastSetIndices(u8 *base, u32 device, u32 ib) {
  u8 *dev = Guest(base, device);
  const u32 old = Ld32(dev + dev::kIndexBuffer);
  if (NeedsRing(base, dev, old))
    return false;
  StampReplaced(base, dev, old);
  St32(dev + dev::kIndexBuffer, ib);
  return true;
}

struct VerifyRegions {
  struct Region {
    u8 *p;
    u32 n;
  };
  Region regions[7];
  u32 count = 0;
  u8 before[128];
  u8 after_fast[128];
  u32 total = 0;
  void add(u8 *p, u32 n) {
    regions[count++] = {p, n};
    total += n;
  }
  void snapshot(u8 *out) const {
    u32 off = 0;
    for (u32 i = 0; i < count; ++i) {
      std::memcpy(out + off, regions[i].p, regions[i].n);
      off += regions[i].n;
    }
  }
  void restore(const u8 *in) const {
    u32 off = 0;
    for (u32 i = 0; i < count; ++i) {
      std::memcpy(regions[i].p, in + off, regions[i].n);
      off += regions[i].n;
    }
  }
};

void ReportVerify(const char *what, const VerifyRegions &v, const u8 *after_orig) {
  static u32 reports = 0;
  if (reports >= 40)
    return;
  u32 off = 0;
  for (u32 i = 0; i < v.count; ++i) {
    if (std::memcmp(v.after_fast + off, after_orig + off, v.regions[i].n) != 0) {
      std::string fast, orig;
      for (u32 b = 0; b < v.regions[i].n; ++b) {
        fast += std::format("{:02x}", v.after_fast[off + b]);
        orig += std::format("{:02x}", after_orig[off + b]);
      }
      EOT_WARN("[fast-setters] {} region {} differs: hook {} xdk {}", what, i, fast, orig);
      reports++;
    }
    off += v.regions[i].n;
  }
}

}

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
  const u32 device = ctx.r3.u32, sampler = ctx.r4.u32, texture = ctx.r5.u32;
  const u64 mask = ctx.r6.u64;
  static const bool fast = (Settings::FastSetters() & 1) != 0;
  static const bool verify = Settings::FastSettersVerify();
  bool done = false;
  if (fast && sampler < 26 && device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      VerifyRegions v;
      v.add(dev + dev::kFetchConstants + 24 * sampler, 24);
      v.add(dev + dev::kTextureObject0 + 4 * sampler, 4);
      v.add(dev + kDevPendingGroup3, 8);
      if (const u32 old = Ld32(dev + dev::kTextureObject0 + 4 * sampler))
        v.add(Guest(base, old) + kObjReleaseStamp, 4);
      v.snapshot(v.before);
      if (FastSetTexture(base, device, sampler, texture, mask)) {
        v.snapshot(v.after_fast);
        v.restore(v.before);
        __imp__D3DDevice_SetTexture(ctx, base);
        u8 after_orig[128];
        v.snapshot(after_orig);
        ReportVerify("SetTexture", v, after_orig);
        done = true;
      }
    } else {
      done = FastSetTexture(base, device, sampler, texture, mask);
    }
  }
  if (!done) {
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
  const u32 device = ctx.r3.u32, shader = ctx.r4.u32;
  static const bool fast = (Settings::FastSetters() & 8) != 0;
  static const bool verify = Settings::FastSettersVerify();
  bool done = false;
  if (fast && device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      DeviceCompare cmp;
      cmp.Snapshot(dev, cmp.before);
      if (FastSetVertexShader(base, device, shader)) {
        cmp.Snapshot(dev, cmp.after_fast);
        cmp.Restore(dev);
        __imp__D3DDevice_SetVertexShader(ctx, base);
        cmp.Snapshot(dev, cmp.after_orig);
        cmp.Report("SetVertexShader", nullptr, 0);
        done = true;
      }
    } else {
      done = FastSetVertexShader(base, device, shader);
    }
  }
  if (!done) {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetVertexShader(ctx, base);
  }
  EOT_TRACE_CALL("SetVertexShader {:#x}", shader);
  trace::Bump(trace::Counter::SetVertexShader);
}

extern "C" REX_FUNC(D3DDevice_SetPixelShader) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, shader = ctx.r4.u32;
  static const bool fast = (Settings::FastSetters() & 8) != 0;
  static const bool verify = Settings::FastSettersVerify();
  bool done = false;
  if (fast && device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      DeviceCompare cmp;
      cmp.Snapshot(dev, cmp.before);
      if (FastSetPixelShader(base, device, shader)) {
        cmp.Snapshot(dev, cmp.after_fast);
        cmp.Restore(dev);
        __imp__D3DDevice_SetPixelShader(ctx, base);
        cmp.Snapshot(dev, cmp.after_orig);
        cmp.Report("SetPixelShader", nullptr, 0);
        done = true;
      }
    } else {
      done = FastSetPixelShader(base, device, shader);
    }
  }
  if (!done) {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetPixelShader(ctx, base);
  }
  EOT_TRACE_CALL("SetPixelShader {:#x}", shader);
  trace::Bump(trace::Counter::SetPixelShader);
}

extern "C" REX_FUNC(D3DDevice_SetStreamSource) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, stream = ctx.r4.u32, vb = ctx.r5.u32, offset = ctx.r6.u32,
            stride = ctx.r7.u32;
  const u64 mask = ctx.r8.u64;
  static const bool fast = (Settings::FastSetters() & 2) != 0;
  static const bool verify = Settings::FastSettersVerify();
  bool done = false;
  if (fast && stream < 16 && device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      VerifyRegions v;
      v.add(dev + dev::StreamFetchSlotOffset(stream), 8);
      v.add(dev + dev::kStreamObject0 + 4 * stream, 4);
      v.add(dev + dev::kStreamStride0 + stream, 1);
      v.add(dev + kDevPendingGroup2, 8);
      v.add(dev + kDevPendingGroup3, 8);
      if (const u32 old = Ld32(dev + dev::kStreamObject0 + 4 * stream))
        v.add(Guest(base, old) + kObjReleaseStamp, 4);
      v.snapshot(v.before);
      if (FastSetStreamSource(base, device, stream, vb, offset, stride, mask)) {
        v.snapshot(v.after_fast);
        v.restore(v.before);
        __imp__D3DDevice_SetStreamSource(ctx, base);
        u8 after_orig[128];
        v.snapshot(after_orig);
        ReportVerify("SetStreamSource", v, after_orig);
        done = true;
      }
    } else {
      done = FastSetStreamSource(base, device, stream, vb, offset, stride, mask);
    }
  }
  if (!done) {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetStreamSource(ctx, base);
  }
  EOT_TRACE_CALL("SetStreamSource {} vb={:#x} offset={} stride={}", stream, vb, offset, stride);
  trace::Bump(trace::Counter::SetStreamSource);
}

extern "C" REX_FUNC(D3DDevice_SetIndices) {
  FlushPendingUpDraw();
  const u32 device = ctx.r3.u32, ib = ctx.r4.u32;
  static const bool fast = (Settings::FastSetters() & 4) != 0;
  static const bool verify = Settings::FastSettersVerify();
  bool done = false;
  if (fast && device) {
    if (verify) {
      u8 *dev = Guest(base, device);
      VerifyRegions v;
      v.add(dev + dev::kIndexBuffer, 4);
      if (const u32 old = Ld32(dev + dev::kIndexBuffer))
        v.add(Guest(base, old) + kObjReleaseStamp, 4);
      v.snapshot(v.before);
      if (FastSetIndices(base, device, ib)) {
        v.snapshot(v.after_fast);
        v.restore(v.before);
        __imp__D3DDevice_SetIndices(ctx, base);
        u8 after_orig[128];
        v.snapshot(after_orig);
        ReportVerify("SetIndices", v, after_orig);
        done = true;
      }
    } else {
      done = FastSetIndices(base, device, ib);
    }
  }
  if (!done) {
    PerfScopeSampled guest_scope(state().perf.guest_d3d_ms, state().perf.guest_d3d_calls);
    __imp__D3DDevice_SetIndices(ctx, base);
  }
  EOT_TRACE_CALL("SetIndices {:#x}", ib);
  trace::Bump(trace::Counter::SetIndices);
}
