#include "gpu/draw.h"

#include "core/profiling.h"

#include "gpu/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/memory/utils.h>

#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/gpu_timing.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/sampler_cache.h"
#include "gpu/settings.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/surfaces.h"
#include "gpu/textures.h"
#include "gpu/trace.h"
#include "gpu/vertex_layout.h"

namespace eot::gpu {

namespace {

namespace xe = rex::graphics::xenos;

constexpr u32 kPrimTriangleStrip = 6;
constexpr u32 kPrimTriangleFan = 5;
constexpr u32 kPrimRectList = 8;
constexpr u32 kPrimQuadList = 13;
constexpr u32 kPrimLineStrip = 3;

struct Targets {
  GuestSurface *color[4] = {};
  u32 colorCount = 0;
  GuestSurface *depth = nullptr;
  u32 width = 0;
  u32 height = 0;
  float scale = 1.0f;
};

struct ViewportInfo {
  plume::RenderViewport vp;
  plume::RenderRect scissor;
  float posScale[4] = {1, 1, 1, 1};
  float posOffset[4] = {0, 0, 0, 0};
};

void Dropped(const char *why, u64 site) {
  trace::Bump(trace::Counter::DrawDropped);
  u32 n;
  if (DiagShouldLog(site, &n))
    EOT_WARN("[draw] dropped: {} (x{})", why, n + 1);
}

bool ResolveTargets(VideoState &s, DeviceView dev, Targets &t) {
  t = Targets{};
  for (u32 i = 0; i < 4; ++i) {
    const u32 va = dev.U32(dev::kRenderTarget0 + 4 * i);
    if (!va)
      break;
    GuestSurface *surf = GetGuestSurface(s, va);
    if (!surf || !surf->host.valid())
      break;
    const u32 packet = dev.U32(i == 0 ? dev::kColor0Info : dev::kColor1Info + 4 * (i - 1));
    const u32 bias = (packet >> 20) & 0x3F;
    surf->colorExpBias = bias & 0x20 ? static_cast<i32>(bias) - 64 : static_cast<i32>(bias);
    t.color[i] = surf;
    t.colorCount = i + 1;
  }
  const u32 ds_va = dev.U32(dev::kDepthSurface);
  if (ds_va)
    t.depth = GetGuestSurface(s, ds_va);
  if (t.depth && !t.depth->host.valid())
    t.depth = nullptr;
  if (t.colorCount) {
    t.width = t.color[0]->width;
    t.height = t.color[0]->height;
  } else if (t.depth) {
    t.width = t.depth->width;
    t.height = t.depth->height;
  }
  if (t.colorCount)
    t.scale = t.color[0]->scale;
  else if (t.depth)
    t.scale = t.depth->scale;
  return t.colorCount || t.depth;
}

bool BindTargets(VideoState &s, Targets &t) {
  HostTexture *colors[4] = {};
  for (u32 i = 0; i < t.colorCount; ++i) {
    colors[i] = &t.color[i]->host;
    TransitionLocked(s, *colors[i], plume::RenderTextureLayout::COLOR_WRITE);
    t.color[i]->drawn = true;
    t.color[i]->lastUseFrame = s.guest_frames;
  }
  HostTexture *depth = nullptr;
  if (t.depth) {
    depth = &t.depth->host;
    TransitionLocked(s, *depth, plume::RenderTextureLayout::DEPTH_WRITE);
    t.depth->drawn = true;
    t.depth->lastUseFrame = s.guest_frames;
  }
  if (depth && t.colorCount && colors[0]->sampleCount != depth->sampleCount) {
    u32 n;
    if (DiagShouldLog(0x6C20, &n) && n < 8)
      EOT_WARN("[draw] colour {}x{} ({}x samples) bound with depth {}x{} ({}x samples)",
               t.color[0]->width, t.color[0]->height, colors[0]->sampleCount, t.depth->width,
               t.depth->height, depth->sampleCount);
  }
  plume::RenderFramebuffer *fb = GetFramebuffer(s, colors, t.colorCount, depth);
  if (!fb)
    return false;
  if (s.bound_framebuffer != fb) {
    s.command_list->setFramebuffer(fb);
    s.bound_framebuffer = fb;
  }
  GpuTimingMark(s, s.command_list,
                GpuTargetCategory(t.width == kGuestRenderWidth && t.height == kGuestRenderHeight,
                                  t.depth != nullptr, t.colorCount,
                                  t.colorCount ? static_cast<u32>(t.color[0]->host.format) : 0u));
  for (u32 i = 0; i < t.colorCount; ++i) {
    if (!colors[i]->needsClear)
      continue;
    s.command_list->clearColor(i, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
    colors[i]->needsClear = false;
  }
  if (depth && depth->needsClear) {
    s.command_list->clearDepthStencil(true, true, 0.0f, 0, nullptr, 0);
    depth->needsClear = false;
  }
  return true;
}

ViewportInfo ComputeViewport(DeviceView dev, const Targets &t) {
  ViewportInfo v;
  const float rt_w = static_cast<float>(std::max(1u, t.width));
  const float rt_h = static_cast<float>(std::max(1u, t.height));
  const u32 vte = dev.U32(dev::kVteControl);
  const float xs = dev.F32(dev::kVportXScale), xo = dev.F32(dev::kVportXOffset);
  const float ys = dev.F32(dev::kVportYScale), yo = dev.F32(dev::kVportYOffset);
  const float zs = dev.F32(dev::kVportZScale), zo = dev.F32(dev::kVportZOffset);
  const bool xs_en = vte & 1, xo_en = vte & 2, ys_en = vte & 4, yo_en = vte & 8;
  const bool zs_en = vte & 16, zo_en = vte & 32;

  float x0, w, y0, h;
  if (xs_en && xs != 0.0f) {
    w = 2.0f * std::fabs(xs);
    x0 = (xo_en ? xo : 0.0f) - std::fabs(xs);
    v.posScale[0] = xs < 0 ? -1.0f : 1.0f;
  } else {
    x0 = 0.0f;
    w = rt_w;
    v.posScale[0] = 2.0f / rt_w;
    v.posOffset[0] = (xo_en ? xo : 0.0f) * 2.0f / rt_w - 1.0f;
  }
  if (ys_en && ys != 0.0f) {
    h = 2.0f * std::fabs(ys);
    y0 = (yo_en ? yo : 0.0f) - std::fabs(ys);
    v.posScale[1] = ys < 0 ? 1.0f : -1.0f;
  } else {
    y0 = 0.0f;
    h = rt_h;
    v.posScale[1] = -2.0f / rt_h;
    v.posOffset[1] = 1.0f - (yo_en ? yo : 0.0f) * 2.0f / rt_h;
  }
  float zmin, zmax;
  if (zs_en) {
    if (zs >= 0.0f) {
      zmin = zo_en ? zo : 0.0f;
      zmax = zmin + zs;
    } else {
      zmax = zo_en ? zo : 0.0f;
      zmin = zmax + zs;
      v.posScale[2] = -1.0f;
      v.posOffset[2] = 1.0f;
    }
  } else {
    zmin = 0.0f;
    zmax = 1.0f;
    v.posOffset[2] = zo_en ? zo : 0.0f;
  }
  zmin = std::clamp(zmin, 0.0f, 1.0f);
  zmax = std::clamp(zmax, 0.0f, 1.0f);
  if ((dev.U32(dev::kVtxControl) & 1) == 0) {
    v.posOffset[0] += 1.0f / std::max(1.0f, w);
    v.posOffset[1] -= 1.0f / std::max(1.0f, h);
  }
  const float S = t.scale;
  const float hx0 = std::round(x0 * S), hx1 = std::round((x0 + w) * S);
  const float hy0 = std::round(y0 * S), hy1 = std::round((y0 + h) * S);
  v.vp = plume::RenderViewport(hx0, hy0, std::max(hx1 - hx0, 1.0f), std::max(hy1 - hy0, 1.0f),
                               zmin, zmax);

  auto scissor_of = [](u32 tl, u32 br) {
    return plume::RenderRect(static_cast<i32>(tl & 0x7FFF), static_cast<i32>((tl >> 16) & 0x7FFF),
                             static_cast<i32>(br & 0x7FFF), static_cast<i32>((br >> 16) & 0x7FFF));
  };
  const plume::RenderRect win =
      scissor_of(dev.U32(dev::kWindowScissorTL), dev.U32(dev::kWindowScissorBR));
  const plume::RenderRect scr =
      scissor_of(dev.U32(dev::kScreenScissorTL), dev.U32(dev::kScreenScissorBR));
  plume::RenderRect sc;
  sc.left = std::max({0, win.left, scr.left});
  sc.top = std::max({0, win.top, scr.top});
  sc.right = std::min({static_cast<i32>(t.width), win.right, scr.right});
  sc.bottom = std::min({static_cast<i32>(t.height), win.bottom, scr.bottom});
  if (sc.right <= sc.left || sc.bottom <= sc.top)
    sc = plume::RenderRect(0, 0, static_cast<i32>(t.width), static_cast<i32>(t.height));
  sc = plume::RenderRect(ScalePxBy(sc.left, S), ScalePxBy(sc.top, S),
                         std::max(ScalePxBy(sc.right, S), ScalePxBy(sc.left, S) + 1),
                         std::max(ScalePxBy(sc.bottom, S), ScalePxBy(sc.top, S) + 1));
  v.scissor = sc;
  return v;
}

struct ConstFileCache {
  alignas(16) u8 bytes[256 * 16] = {};
  UploadAlloc alloc;
  u64 epoch = ~0ull;
  u32 regs = 0;
};

bool UploadFloatFile(VideoState &s, DeviceView dev, u32 offset, u32 stage, u32 regs,
                     UploadAlloc *out) {
  static ConstFileCache caches[2];
  static const bool ranged = Settings::ConstRange();
  regs = ranged ? std::clamp(regs, 16u, 256u) : 256u;
  const u32 bytes = regs * 16;
  const u8 *src = dev.Bytes(offset, bytes);
  if (!src)
    return false;
  ConstFileCache &c = caches[stage & 1];
  if (c.epoch == UploadRingEpoch() && c.regs >= regs && std::memcmp(c.bytes, src, bytes) == 0) {
    *out = c.alloc;
    s.perf.const_file_hits++;
    return true;
  }
  if (!UploadAllocate(bytes, kConstantBufferAlignment, out))
    return false;
  rex::memory::copy_and_swap_32_unaligned(out->cpu, reinterpret_cast<const u32 *>(src), regs * 4);
  std::memcpy(c.bytes, src, bytes);
  c.alloc = *out;
  c.epoch = UploadRingEpoch();
  c.regs = regs;
  s.perf.constant_bytes += bytes;
  return true;
}

void FillLoopConstants(DeviceView dev, u32 offset, i32 (*dst)[4]) {
  for (u32 i = 0; i < 16; ++i) {
    const u32 v = dev.U32(offset + 4 * i);
    dst[i][0] = static_cast<i32>(v & 0xFF);
    dst[i][1] = static_cast<i32>((v >> 8) & 0xFF);
    dst[i][2] = static_cast<i32>(static_cast<i8>((v >> 16) & 0xFF));
    dst[i][3] = 0;
  }
}

struct CachedIndexRange {
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u32 count = 0;
  u32 lo = 0, hi = 0;
  bool is32 = false;
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  u64 lastUseFrame = 0;
};

struct GeometryPlan {
  const CachedIndexRange *cached = nullptr;
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  bool indexed = false;
  std::vector<u32> indices;
  u32 vertexCount = 0;
  u32 startVertex = 0;
  u32 minVertex = 0;
  u32 maxVertex = 0;
  i32 baseVertex = 0;
  bool rectList = false;
  u32 stream0OverrideVa = 0;
  u32 stream0OverrideStride = 0;
};

bool ReadGuestIndices(u32 ib_va, u32 start_index, u32 count, std::vector<u32> &out) {
  if (!ib_va || !count)
    return false;
  const u32 common = mem::load<u32>(ib_va + obj::kCommon);
  const u32 base = mem::load<u32>(ib_va + obj::kBufferFetch0);
  const u32 size = mem::load<u32>(ib_va + obj::kBufferFetch1);
  const bool is32 = (common & obj::kIndexBuffer32BitBit) != 0;
  const u32 elem = is32 ? 4 : 2;
  if (!base)
    return false;
  if (size && (u64(start_index) + count) * elem > size) {
    u32 n;
    if (DiagShouldLog(0x6100, &n))
      EOT_WARN("[draw] index range {}+{} exceeds buffer {:#x} size {}", start_index, count, ib_va,
               size);
    count = static_cast<u32>(size / elem) > start_index ? size / elem - start_index : 0;
    if (!count)
      return false;
  }
  out.resize(count);
  if (is32) {
    auto *p = mem::at<be_u32>(base + start_index * 4);
    if (!p)
      return false;
    for (u32 i = 0; i < count; ++i)
      out[i] = p[i];
  } else {
    auto *p = mem::at<be_u16>(base + start_index * 2);
    if (!p)
      return false;
    for (u32 i = 0; i < count; ++i)
      out[i] = p[i];
  }
  return true;
}

void ExpandIndices(u32 prim, std::vector<u32> &idx, bool has_restart) {
  std::vector<u32> out;
  switch (prim) {
  case kPrimTriangleStrip: {
    out.reserve(idx.size() * 3);
    u32 run = 0;
    for (size_t i = 0; i < idx.size(); ++i) {
      if (has_restart && (idx[i] == 0xFFFF || idx[i] == 0xFFFFFFFFu)) {
        run = 0;
        continue;
      }
      ++run;
      if (run >= 3) {
        const u32 a = idx[i - 2], b = idx[i - 1], c = idx[i];
        if ((run & 1) == 1) {
          out.push_back(a);
          out.push_back(b);
          out.push_back(c);
        } else {
          out.push_back(b);
          out.push_back(a);
          out.push_back(c);
        }
      }
    }
    break;
  }
  case kPrimTriangleFan: {
    out.reserve(idx.size() * 3);
    for (size_t i = 2; i < idx.size(); ++i) {
      out.push_back(idx[0]);
      out.push_back(idx[i - 1]);
      out.push_back(idx[i]);
    }
    break;
  }
  case kPrimQuadList: {
    out.reserve(idx.size() / 4 * 6);
    for (size_t i = 0; i + 3 < idx.size(); i += 4) {
      out.push_back(idx[i]);
      out.push_back(idx[i + 1]);
      out.push_back(idx[i + 2]);
      out.push_back(idx[i]);
      out.push_back(idx[i + 2]);
      out.push_back(idx[i + 3]);
    }
    break;
  }
  case kPrimLineStrip: {
    out.reserve(idx.size() * 2);
    for (size_t i = 1; i < idx.size(); ++i) {
      if (has_restart && (idx[i] == 0xFFFF || idx[i - 1] == 0xFFFF))
        continue;
      out.push_back(idx[i - 1]);
      out.push_back(idx[i]);
    }
    break;
  }
  default:
    return;
  }
  idx.swap(out);
}

struct StreamInfo {
  u32 stream = 0;
  u32 stride = 0;
  const u8 *data = nullptr;
  u32 sizeBytes = 0;
  u32 dataVa = 0;
  u32 objectVa = 0;
};

bool ReadStream(DeviceView dev, u32 stream, StreamInfo &out) {
  out.stream = stream;
  out.stride = dev.U8(dev::kStreamStride0 + stream) * 4u;
  const u32 slot = dev::StreamFetchSlotOffset(stream);
  const u32 d0 = dev.U32(slot);
  const u32 d1 = dev.U32(slot + 4);
  if ((d0 & 3) != 3 || !out.stride)
    return false;
  out.data = mem::phys<u8>(d0 & ~3u);
  out.sizeBytes = ((d1 >> 2) & 0xFFFFFF) * 4u;
  out.dataVa = d0 & ~3u;
  out.objectVa = dev.U32(dev::kStreamObject0 + 4 * stream);
  return out.data != nullptr;
}

void CopyVertexBytes(u8 *dst, const u8 *src, u32 bytes) {
  const u32 dwords = bytes / 4;
  rex::memory::copy_and_swap_32_unaligned(dst, src, dwords);
  if (bytes & 3)
    std::memcpy(dst + dwords * 4, src + dwords * 4, bytes & 3);
}

struct IndexCacheKey {
  u32 ib_va = 0, data_va = 0, start = 0, count = 0, prim = 0;
  u64 seq = 0, sample = 0;
  bool operator==(const IndexCacheKey &o) const {
    return ib_va == o.ib_va && data_va == o.data_va && start == o.start && count == o.count &&
           prim == o.prim && seq == o.seq && sample == o.sample;
  }
};
struct IndexCacheKeyHash {
  size_t operator()(const IndexCacheKey &k) const {
    u64 h = 0x9E3779B97F4A7C15ull;
    auto mix = [&](u64 v) {
      h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
    };
    mix(k.ib_va);
    mix(k.data_va);
    mix((u64(k.start) << 32) | k.count);
    mix(k.prim);
    mix(k.seq);
    mix(k.sample);
    return static_cast<size_t>(h);
  }
};
struct IndexCacheChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *cpu = nullptr;
  u64 capacity = 0, used = 0;
};
struct IndexCache {
  std::unordered_map<IndexCacheKey, CachedIndexRange, IndexCacheKeyHash> map;
  std::vector<IndexCacheChunk> chunks;
  u64 totalBytes = 0;
};
IndexCache &index_cache() {
  static IndexCache c;
  return c;
}
constexpr u64 kIndexCacheChunkBytes = 8ull << 20;
constexpr u64 kIndexCacheBudgetBytes = 96ull << 20;

u64 SampleHostBytes(const u8 *p, u32 bytes) {
  if (!p || bytes < 4)
    return 0;
  u64 h = 1469598103934665603ull;
  auto mix = [&](u32 off) {
    u32 v;
    std::memcpy(&v, p + off, 4);
    h ^= v;
    h *= 1099511628211ull;
  };
  const u32 span = bytes >= 16 ? bytes - 16 : 0;
  for (u32 k = 0; k < 8; ++k) {
    const u32 base = static_cast<u32>((u64(span) * k / 7) & ~3ull);
    for (u32 i = 0; i < 16 && base + i + 4 <= bytes; i += 4)
      mix(base + i);
  }
  return h;
}
u64 SampleGuestBytes(u32 va, u32 bytes) { return SampleHostBytes(mem::at<u8>(va), bytes); }

struct BufferPool {
  std::vector<IndexCacheChunk> chunks;
  u64 totalBytes = 0;
};
bool PoolAllocate(VideoState &s, BufferPool &pool, u64 budget, u64 chunk_bytes,
                  plume::RenderBufferFlags flags, const char *name, u64 bytes, u64 align,
                  plume::RenderBuffer **buffer, u64 *offset, u8 **cpu, bool *reset) {
  *reset = false;
  if (pool.totalBytes + bytes > budget) {
    for (auto &ch : pool.chunks)
      ParkBuffer(s, std::move(ch.buffer));
    pool.chunks.clear();
    pool.totalBytes = 0;
    *reset = true;
    EOT_INFO("[draw] {} pool over budget ({} MB); rebuilt", name, budget >> 20);
  }
  if (pool.chunks.empty() ||
      ((pool.chunks.back().used + align - 1) & ~(align - 1)) + bytes > pool.chunks.back().capacity) {
    IndexCacheChunk ch;
    const u64 size = std::max(chunk_bytes, bytes + align);
    plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(size);
    desc.flags = flags;
    ch.buffer = CreateHostBuffer(s.device.get(), desc, name);
    if (!ch.buffer)
      return false;
    ch.cpu = static_cast<u8 *>(ch.buffer->map());
    if (!ch.cpu) {
      ch.buffer.reset();
      return false;
    }
    ch.capacity = size;
    pool.chunks.push_back(std::move(ch));
  }
  auto &ch = pool.chunks.back();
  const u64 off = (ch.used + align - 1) & ~(align - 1);
  *buffer = ch.buffer.get();
  *offset = off;
  *cpu = ch.cpu + off;
  ch.used = off + bytes;
  pool.totalBytes += bytes;
  return true;
}

BufferPool &index_pool() {
  static BufferPool p;
  return p;
}
bool IndexCacheAllocate(VideoState &s, u64 bytes, plume::RenderBuffer **buffer, u64 *offset,
                        u8 **cpu) {
  bool reset = false;
  const bool ok = PoolAllocate(s, index_pool(), kIndexCacheBudgetBytes, kIndexCacheChunkBytes,
                               plume::RenderBufferFlag::INDEX, "index-cache", bytes, 4, buffer,
                               offset, cpu, &reset);
  if (reset)
    index_cache().map.clear();
  return ok;
}

struct VertexMirrorKey {
  u32 data_va = 0, size = 0;
  u64 seq = 0, sample = 0;
  bool operator==(const VertexMirrorKey &o) const {
    return data_va == o.data_va && size == o.size && seq == o.seq && sample == o.sample;
  }
};
struct VertexMirrorKeyHash {
  size_t operator()(const VertexMirrorKey &k) const {
    u64 h = 0x9E3779B97F4A7C15ull;
    auto mix = [&](u64 v) {
      h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      h *= 0xFF51AFD7ED558CCDull;
    };
    mix((u64(k.data_va) << 32) | k.size);
    mix(k.seq);
    mix(k.sample);
    return static_cast<size_t>(h);
  }
};
struct VertexMirror {
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u32 size = 0;
  u64 lastUseFrame = 0;
};
struct VertexMirrorCache {
  std::unordered_map<VertexMirrorKey, VertexMirror, VertexMirrorKeyHash> map;
  std::unordered_map<VertexMirrorKey, u64, VertexMirrorKeyHash> seen;
  BufferPool pool;
  bool admissionClosed = false;
};
VertexMirrorCache &vertex_mirrors() {
  static VertexMirrorCache c;
  return c;
}
constexpr u64 kVertexMirrorChunkBytes = 16ull << 20;
constexpr u64 kVertexMirrorBudgetBytes = 256ull << 20;
constexpr u32 kVertexMirrorMaxBytes = 32u << 20;
constexpr size_t kVertexMirrorSeenCap = 4096;

const VertexMirror *GetVertexMirror(VideoState &s, const StreamInfo &st, u64 first, u64 bytes) {
  static const bool enabled = Settings::VertexMirrors();
  if (!enabled || !st.dataVa || !bytes || bytes > kVertexMirrorMaxBytes || !st.data ||
      first > UINT32_MAX - st.dataVa)
    return nullptr;
  auto &c = vertex_mirrors();
  VertexMirrorKey key;
  key.data_va = st.dataVa + static_cast<u32>(first);
  key.size = static_cast<u32>(bytes);
  key.seq = st.objectVa ? ResourceUnlockSeq(st.objectVa) : 0;
  key.sample = SampleHostBytes(st.data + first, key.size);
  auto it = c.map.find(key);
  if (it != c.map.end()) {
    it->second.lastUseFrame = s.guest_frames;
    s.perf.vertex_cache_hits++;
    return &it->second;
  }
  if (c.admissionClosed)
    return nullptr;
  auto seen = c.seen.find(key);
  if (seen == c.seen.end()) {
    if (c.seen.size() >= kVertexMirrorSeenCap)
      c.seen.clear();
    c.seen.emplace(key, s.guest_frames);
    return nullptr;
  }
  if (seen->second >= s.guest_frames)
    return nullptr;
  c.seen.erase(seen);
  if (c.pool.totalBytes > kVertexMirrorBudgetBytes ||
      bytes > kVertexMirrorBudgetBytes - c.pool.totalBytes) {
    c.admissionClosed = true;
    EOT_INFO("[draw] vertex-mirror cache full ({} MB, {} ranges); keeping residents and "
             "using frame uploads for new ranges",
             c.pool.totalBytes >> 20, c.map.size());
    return nullptr;
  }
  s.perf.vertex_cache_misses++;
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  bool reset = false;
  const bool ok = PoolAllocate(s, c.pool, kVertexMirrorBudgetBytes, kVertexMirrorChunkBytes,
                               plume::RenderBufferFlag::VERTEX, "vertex-mirror", bytes, 16,
                               &buffer, &offset, &cpu, &reset);
  if (reset)
    c.map.clear();
  if (!ok)
    return nullptr;
  {
    PerfScope copy_scope(s.perf.vertex_copy_ms);
    CopyVertexBytes(cpu, st.data + first, static_cast<u32>(bytes));
  }
  s.perf.vertex_bytes += bytes;
  VertexMirror m;
  m.buffer = buffer;
  m.offset = offset;
  m.size = static_cast<u32>(bytes);
  m.lastUseFrame = s.guest_frames;
  return &c.map.emplace(key, m).first->second;
}

const CachedIndexRange *GetCachedIndexRange(VideoState &s, u32 ib_va, u32 prim, u32 start_index,
                                            u32 count) {
  if (!ib_va || !count || prim == kPrimRectList)
    return nullptr;
  const u32 common = mem::load<u32>(ib_va + obj::kCommon);
  const u32 data_va = mem::load<u32>(ib_va + obj::kBufferFetch0);
  const u32 size = mem::load<u32>(ib_va + obj::kBufferFetch1);
  if (!data_va)
    return nullptr;
  const bool is32 = (common & obj::kIndexBuffer32BitBit) != 0;
  const u32 elem = is32 ? 4 : 2;
  const u64 range_start = u64(start_index) * elem;
  u64 range_bytes = u64(count) * elem;
  if (size && range_start + range_bytes > size)
    range_bytes = range_start < size ? size - range_start : 0;
  if (!range_bytes)
    return nullptr;
  IndexCacheKey key;
  key.ib_va = ib_va;
  key.data_va = data_va;
  key.start = start_index;
  key.count = count;
  key.prim = prim;
  key.seq = ResourceUnlockSeq(ib_va);
  key.sample = SampleGuestBytes(data_va + static_cast<u32>(range_start), static_cast<u32>(range_bytes));

  auto &c = index_cache();
  auto it = c.map.find(key);
  if (it != c.map.end()) {
    it->second.lastUseFrame = s.guest_frames;
    s.perf.index_cache_hits++;
    return &it->second;
  }
  s.perf.index_cache_misses++;

  std::vector<u32> idx;
  if (!ReadGuestIndices(ib_va, start_index, count, idx))
    return nullptr;
  bool expand = false;
  plume::RenderPrimitiveTopology topology = ConvertPrimitiveType(prim, &expand);
  if (expand || prim == kPrimTriangleStrip || prim == kPrimLineStrip) {
    ExpandIndices(prim, idx, true);
    topology = prim == kPrimLineStrip ? plume::RenderPrimitiveTopology::LINE_LIST
                                      : plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  if (idx.empty())
    return nullptr;
  u32 lo = 0xFFFFFFFFu, hi = 0;
  for (u32 v : idx) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  const bool host32 = hi > 0xFFFEu;
  const u64 bytes = idx.size() * (host32 ? 4 : 2);
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u8 *cpu = nullptr;
  {
    std::lock_guard lock(s.mutex);
    if (!s.ready || !IndexCacheAllocate(s, bytes, &buffer, &offset, &cpu))
      return nullptr;
  }
  if (host32) {
    std::memcpy(cpu, idx.data(), bytes);
  } else {
    auto *dst = reinterpret_cast<u16 *>(cpu);
    for (size_t i = 0; i < idx.size(); ++i)
      dst[i] = static_cast<u16>(idx[i]);
  }
  CachedIndexRange r;
  r.buffer = buffer;
  r.offset = offset;
  r.count = static_cast<u32>(idx.size());
  r.lo = lo;
  r.hi = hi;
  r.is32 = host32;
  r.topology = topology;
  r.lastUseFrame = s.guest_frames;
  return &(c.map[key] = r);
}

struct RectExpansion {
  std::vector<u8> vertices[16];
  std::vector<u32> indices;
  u32 hostVertexCount = 0;
};

float LaneToFloat(const u8 *p, plume::RenderFormat f, u32 lane, bool *ok) {
  using F = plume::RenderFormat;
  *ok = true;
  switch (f) {
  case F::R32_FLOAT:
  case F::R32G32_FLOAT:
  case F::R32G32B32_FLOAT:
  case F::R32G32B32A32_FLOAT: {
    float v;
    std::memcpy(&v, p + lane * 4, 4);
    return v;
  }
  case F::R16G16_FLOAT:
  case F::R16G16B16A16_FLOAT:
  case F::R16_FLOAT: {
    u16 h;
    std::memcpy(&h, p + lane * 2, 2);
    const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    float v;
    if (exp == 0)
      v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31)
      v = man ? NAN : INFINITY;
    else
      v = std::ldexp(static_cast<float>(man | 0x400), static_cast<int>(exp) - 25);
    return sign ? -v : v;
  }
  default:
    *ok = false;
    return 0.0f;
  }
}

void SynthesizeLane(u8 *dst, const u8 *vi, const u8 *vj, const u8 *vk, plume::RenderFormat f,
                    u32 bytes) {
  using F = plume::RenderFormat;
  auto lanes_of = [](F fmt, u32 *lane_bytes) -> u32 {
    switch (fmt) {
    case F::R32_FLOAT:
      *lane_bytes = 4;
      return 1;
    case F::R32G32_FLOAT:
      *lane_bytes = 4;
      return 2;
    case F::R32G32B32_FLOAT:
      *lane_bytes = 4;
      return 3;
    case F::R32G32B32A32_FLOAT:
      *lane_bytes = 4;
      return 4;
    case F::R16_FLOAT:
    case F::R16_UNORM:
    case F::R16_SNORM:
      *lane_bytes = 2;
      return 1;
    case F::R16G16_FLOAT:
    case F::R16G16_UNORM:
    case F::R16G16_SNORM:
    case F::R16G16_UINT:
      *lane_bytes = 2;
      return 2;
    case F::R16G16B16A16_FLOAT:
    case F::R16G16B16A16_UNORM:
    case F::R16G16B16A16_SNORM:
    case F::R16G16B16A16_UINT:
      *lane_bytes = 2;
      return 4;
    case F::R8G8B8A8_UNORM:
    case F::B8G8R8A8_UNORM:
    case F::R8G8B8A8_SNORM:
    case F::R8G8B8A8_UINT:
      *lane_bytes = 1;
      return 4;
    case F::R8G8_UNORM:
    case F::R8G8_SNORM:
    case F::R8G8_UINT:
      *lane_bytes = 1;
      return 2;
    case F::R8_UNORM:
    case F::R8_SNORM:
    case F::R8_UINT:
      *lane_bytes = 1;
      return 1;
    default:
      *lane_bytes = 0;
      return 0;
    }
  };
  u32 lane_bytes = 0;
  const u32 lanes = lanes_of(f, &lane_bytes);
  if (!lanes) {
    std::memcpy(dst, vi, bytes);
    return;
  }
  for (u32 l = 0; l < lanes; ++l) {
    bool ok = false;
    if (lane_bytes == 4 || (lane_bytes == 2 && (f == F::R16_FLOAT || f == F::R16G16_FLOAT ||
                                                 f == F::R16G16B16A16_FLOAT))) {
      const float a = LaneToFloat(vi, f, l, &ok), b = LaneToFloat(vj, f, l, &ok),
                  c = LaneToFloat(vk, f, l, &ok);
      const float r = a + b - c;
      if (lane_bytes == 4) {
        std::memcpy(dst + l * 4, &r, 4);
      } else {
        std::memcpy(dst + l * 2, vi + l * 2, 2);
      }
    } else if (lane_bytes == 2) {
      u16 a, b, c;
      std::memcpy(&a, vi + l * 2, 2);
      std::memcpy(&b, vj + l * 2, 2);
      std::memcpy(&c, vk + l * 2, 2);
      const i32 r = std::clamp(static_cast<i32>(a) + b - c, 0, 65535);
      const u16 rv = static_cast<u16>(r);
      std::memcpy(dst + l * 2, &rv, 2);
    } else {
      const i32 r = std::clamp(static_cast<i32>(vi[l]) + vj[l] - vk[l], 0, 255);
      dst[l] = static_cast<u8>(r);
    }
  }
}

bool ExpandRectList(const InputLayout &layout, const StreamInfo streams[16], u32 stream_mask,
                    const std::vector<u32> &guest_vertices, RectExpansion &out) {
  const plume::RenderInputElement *pos = nullptr;
  for (const auto &e : layout.elements) {
    if (std::strcmp(e.semanticName, "POSITION") == 0 && e.semanticIndex == 0 &&
        e.slotIndex != kSyntheticVertexSlot) {
      pos = &e;
      break;
    }
  }
  const u32 rects = static_cast<u32>(guest_vertices.size() / 3);
  out.hostVertexCount = rects * 4;
  out.indices.reserve(rects * 6);
  for (u32 S = 0; S < 16; ++S) {
    if (stream_mask & (1u << S))
      out.vertices[S].resize(size_t(out.hostVertexCount) * streams[S].stride);
  }
  for (u32 r = 0; r < rects; ++r) {
    const u32 g[3] = {guest_vertices[r * 3], guest_vertices[r * 3 + 1],
                      guest_vertices[r * 3 + 2]};
    u32 k = 0;
    if (pos) {
      const StreamInfo &ps = streams[pos->slotIndex];
      float px[3], py[3];
      bool ok = true;
      for (u32 n = 0; n < 3 && ok; ++n) {
        const u8 *v = ps.data + u64(g[n]) * ps.stride + pos->alignedByteOffset;
        if ((u64(g[n]) + 1) * ps.stride > ps.sizeBytes && ps.sizeBytes) {
          ok = false;
          break;
        }
        u8 tmp[16];
        CopyVertexBytes(tmp, v, 16);
        px[n] = LaneToFloat(tmp, pos->format, 0, &ok);
        py[n] = LaneToFloat(tmp, pos->format, 1, &ok);
      }
      if (ok) {
        float best = INFINITY;
        for (u32 c = 0; c < 3; ++c) {
          const u32 i = (c + 1) % 3, j = (c + 2) % 3;
          const float ax = px[i] - px[c], ay = py[i] - py[c];
          const float bx = px[j] - px[c], by = py[j] - py[c];
          const float dot = std::fabs(ax * bx + ay * by);
          const float norm = std::sqrt((ax * ax + ay * ay) * (bx * bx + by * by)) + 1e-20f;
          const float score = dot / norm;
          if (score < best) {
            best = score;
            k = c;
          }
        }
      }
    }
    const u32 i = (k + 1) % 3, j = (k + 2) % 3;
    const u32 base = r * 4;
    for (u32 S = 0; S < 16; ++S) {
      if (!(stream_mask & (1u << S)))
        continue;
      const StreamInfo &st = streams[S];
      u8 *dst = out.vertices[S].data() + size_t(base) * st.stride;
      for (u32 n = 0; n < 3; ++n) {
        const u8 *src = st.data + u64(g[n]) * st.stride;
        CopyVertexBytes(dst + n * st.stride, src, st.stride);
      }
      u8 *v3 = dst + 3 * st.stride;
      std::memcpy(v3, dst + i * st.stride, st.stride);
      for (const auto &e : layout.elements) {
        if (e.slotIndex != S)
          continue;
        const u32 bytes = std::min(16u, st.stride - e.alignedByteOffset);
        SynthesizeLane(v3 + e.alignedByteOffset, dst + i * st.stride + e.alignedByteOffset,
                       dst + j * st.stride + e.alignedByteOffset,
                       dst + k * st.stride + e.alignedByteOffset, e.format, bytes);
      }
    }
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + i);
    out.indices.push_back(base + 3);
    out.indices.push_back(base + j);
  }
  return true;
}

void FillPipelineState(DeviceView dev, const Targets &t, PipelineState &st,
                       u32 *spec_out, bool *alpha_to_coverage_only) {
  const u32 mode = dev.U32(dev::kModeControl);
  const bool cull_front = mode & 1, cull_back = mode & 2, face_cw = mode & 4;
  if (cull_front && cull_back)
    st.cull = plume::RenderCullMode::BACK;
  else if (cull_front)
    st.cull = plume::RenderCullMode::FRONT;
  else if (cull_back)
    st.cull = plume::RenderCullMode::BACK;
  else
    st.cull = plume::RenderCullMode::NONE;
  st.frontFace = face_cw ? plume::RenderFrontFace::CLOCKWISE
                         : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  const u32 clip = dev.U32(dev::kClipControl);
  st.depthClip = (clip & ((1u << 16) | (1u << 26) | (1u << 27))) == 0;
  if (!st.depthClip) {
    u32 n;
    if (DiagShouldLog(0x6C1F, &n) && n < 3)
      EOT_INFO("[draw] clip control {:#x}: depth clip off", clip);
  }

  if (mode & (1u << 11)) {
    const float scale = dev.F32(dev::kPolyOffsetFrontScale);
    const float offset = dev.F32(dev::kPolyOffsetFrontOffset);
    st.slopeScaledDepthBias = scale / 16.0f;
    st.depthBias = PolygonOffsetUnits(offset);
  }
  const float rs = RenderScaleFactor();
  const float ts = std::fabs(t.scale - rs) < 0.01f ? rs : t.scale;
  st.targetScale = (st.depthBias || st.slopeScaledDepthBias != 0.0f) ? ts : 1.0f;

  const u32 dc = dev.U32(dev::kDepthControl);
  const bool has_ds = t.depth != nullptr;
  const bool z_test = has_ds && (dc & 2);
  st.depthWrite = has_ds && (dc & 4);
  st.depthEnable = z_test || st.depthWrite;
  st.depthFunc = z_test ? ConvertCompareFunc((dc >> 4) & 7)
                        : plume::RenderComparisonFunction::ALWAYS;
  st.stencilEnable = has_ds && (dc & 1);
  if (st.stencilEnable) {
    const u32 srm = dev.U32(dev::kStencilRefMask);
    st.stencilReadMask = static_cast<u8>((srm >> 8) & 0xFF);
    st.stencilWriteMask = static_cast<u8>((srm >> 16) & 0xFF);
    st.stencilRef = static_cast<u8>(srm & 0xFF);
    st.stencilFront.compareFunction = ConvertCompareFunc((dc >> 8) & 7);
    st.stencilFront.failOp = ConvertStencilOp((dc >> 11) & 7);
    st.stencilFront.passOp = ConvertStencilOp((dc >> 14) & 7);
    st.stencilFront.depthFailOp = ConvertStencilOp((dc >> 17) & 7);
    if (dc & (1u << 7)) {
      st.stencilBack.compareFunction = ConvertCompareFunc((dc >> 20) & 7);
      st.stencilBack.failOp = ConvertStencilOp((dc >> 23) & 7);
      st.stencilBack.passOp = ConvertStencilOp((dc >> 26) & 7);
      st.stencilBack.depthFailOp = ConvertStencilOp((dc >> 29) & 7);
    } else {
      st.stencilBack = st.stencilFront;
    }
  } else {
    st.stencilFront.compareFunction = plume::RenderComparisonFunction::ALWAYS;
    st.stencilBack.compareFunction = plume::RenderComparisonFunction::ALWAYS;
  }

  const u32 cc = dev.U32(dev::kColorControl);
  const bool blend_disable = (cc & (1u << 5)) != 0;
  const u32 color_mask = dev.U32(dev::kColorMask);
  static const u32 kBlendRegs[4] = {dev::kBlendControl0, dev::kBlendControl1,
                                    dev::kBlendControl2, dev::kBlendControl3};
  for (u32 i = 0; i < t.colorCount; ++i) {
    const u32 bc = dev.U32(kBlendRegs[i]);
    const u32 src = bc & 0x1F, op = (bc >> 5) & 7, dst = (bc >> 8) & 0x1F;
    const u32 srca = (bc >> 16) & 0x1F, opa = (bc >> 21) & 7, dsta = (bc >> 24) & 0x1F;
    const bool passthrough = src == 1 && dst == 0 && op == 0 && srca == 1 && dsta == 0 && opa == 0;
    plume::RenderBlendDesc &b = st.blend[i];
    b.blendEnabled = !blend_disable && !passthrough;
    b.srcBlend = ConvertBlendFactor(src);
    b.dstBlend = ConvertBlendFactor(dst);
    b.blendOp = ConvertBlendOp(op);
    b.srcBlendAlpha = ConvertBlendFactor(srca);
    b.dstBlendAlpha = ConvertBlendFactor(dsta);
    b.blendOpAlpha = ConvertBlendOp(opa);
    b.renderTargetWriteMask = static_cast<u8>((color_mask >> (4 * i)) & 0xF);
    st.rtFormats[i] = t.color[i]->host.format;
  }
  st.rtCount = t.colorCount;
  st.dsFormat = has_ds ? t.depth->host.format : plume::RenderFormat::UNKNOWN;
  st.sampleCount = t.colorCount ? t.color[0]->host.sampleCount
                                : (t.depth ? t.depth->host.sampleCount : 1u);
  st.alphaToCoverage = (cc & (1u << 4)) != 0;
  *alpha_to_coverage_only = st.alphaToCoverage;

  u32 spec = 0;
  if (cc & (1u << 3)) {
    const u32 func = cc & 7;
    if (func == 4 || func == 6) {
      spec |= kSpecAlphaTest;
    } else if (func == 0) {
      for (u32 i = 0; i < t.colorCount; ++i)
        st.blend[i].renderTargetWriteMask = 0;
    } else if (func != 7) {
      u32 n;
      if (DiagShouldLog(0x6200 + func, &n))
        EOT_WARN("[draw] alpha test function {} not modelled (treated as pass)", func);
    }
  }
  *spec_out = spec;
}

struct SamplerBindings {
  SharedConstants shared;
};

void BindTexturesAndSamplers(VideoState &s, DeviceView dev, SharedConstants &sc) {
  for (u32 i = 0; i < 16; ++i) {
    sc.texture2DIndices[i] = kNullTexture2DDescriptorIndex;
    sc.texture3DIndices[i] = kNullTexture3DDescriptorIndex;
    sc.textureCubeIndices[i] = kNullTextureCubeDescriptorIndex;
    sc.texture1DIndices[i] = kNullTexture2DDescriptorIndex;
    sc.samplerIndices[i] = kSamplerLinearClamp;
  }
  for (u32 slot = 0; slot < 16; ++slot) {
    const u32 tex_va = dev.U32(dev::kTextureObject0 + 4 * slot);
    if (!tex_va)
      continue;
    u32 fc[6];
    for (u32 d = 0; d < 6; ++d)
      fc[d] = dev.U32(dev::kFetchConstants + 24 * slot + 4 * d);
    if ((fc[0] & 3) != 2)
      continue;
    GuestTexture *gt = nullptr;
    u32 index = kInvalidDescriptorIndex;
    u32 sampler = 0;
    VideoState::TextureSlotCache &cs = s.slot_cache[slot];
    const u64 generation = s.texture_generation.load(std::memory_order_relaxed);
    if (cs.generation == generation && cs.texVa == tex_va && cs.texture &&
        std::memcmp(cs.fc, fc, sizeof(fc)) == 0) {
      gt = cs.texture;
      gt->lastUseFrame = s.guest_frames;
      gt->lastSampledFrame = s.guest_frames;
      TransitionLocked(s, gt->host, plume::RenderTextureLayout::SHADER_READ);
      index = cs.index;
      sampler = cs.sampler;
    } else {
      gt = GetGuestTexture(s, tex_va);
      if (!gt) {
        u32 n;
        if (DiagShouldLog(0x6300 ^ tex_va, &n))
          EOT_WARN("[draw] slot {} texture {:#x} has no host mirror (fetch fmt {}); sampling null",
                   slot, tex_va, (fc[1] >> 0) & 0x3F);
        continue;
      }
      const u32 swizzle = (fc[3] >> 1) & 0xFFF;
      gt->lastSampledFrame = s.guest_frames;
      index = PrepareTextureForSampling(s, *gt, swizzle);
      if (index == kInvalidDescriptorIndex) {
        u32 n;
        if (DiagShouldLog(0x6380 ^ tex_va, &n))
          EOT_WARN("[draw] slot {} texture {:#x} (fmt {} {}x{} {}) could not be bound; sampling null",
                   slot, tex_va, static_cast<u32>(gt->format), gt->width, gt->height,
                   gt->host.isDepth ? "depth" : "colour");
        continue;
      }
      sampler = ResolveSamplerSlotLocked(DecodeSamplerFromFetch(fc));
      cs.texVa = tex_va;
      std::memcpy(cs.fc, fc, sizeof(fc));
      cs.generation = s.texture_generation.load(std::memory_order_relaxed);
      cs.texture = gt;
      cs.index = index;
      cs.sampler = sampler;
    }
    switch (gt->dimension) {
    case xe::DataDimension::k3D:
      sc.texture3DIndices[slot] = index;
      break;
    case xe::DataDimension::kCube:
      sc.textureCubeIndices[slot] = index;
      break;
    default:
      sc.texture2DIndices[slot] = index;
      sc.texture1DIndices[slot] = index;
      break;
    }
    sc.samplerIndices[slot] = sampler;
    const u32 sign_x = (fc[0] >> 2) & 3;
    const u32 sign_w = (fc[0] >> 8) & 3;
    if (sign_x == 2)
      sc.biasedTextures |= 1u << slot;
    if (sign_x == 3) {
      const bool srgb_view = gt->host.viewFormat == plume::RenderFormat::BC1_UNORM_SRGB ||
                             gt->host.viewFormat == plume::RenderFormat::BC2_UNORM_SRGB ||
                             gt->host.viewFormat == plume::RenderFormat::BC3_UNORM_SRGB;
      if (!srgb_view)
        sc.biasedTextures |= 1u << (16 + slot);
    }
    if (sign_w == 2)
      sc.sintTexcoords |= 1u << (16 + slot);
  }
}

bool UploadZeroBuffer(VideoState &s, UploadAlloc *out) {
  static UploadAlloc cached[kNumFrames];
  static u64 cached_frame[kNumFrames] = {};
  static bool cached_valid[kNumFrames] = {};
  const u32 slot = s.recording_slot();
  if (cached_valid[slot] && cached_frame[slot] == UploadRingEpoch()) {
    *out = cached[slot];
    return true;
  }
  if (!UploadAllocate(4096, 16, out))
    return false;
  std::memset(out->cpu, 0, 4096);
  cached[slot] = *out;
  cached_frame[slot] = UploadRingEpoch();
  cached_valid[slot] = true;
  return true;
}

void ExecuteDraw(u32 device_va, u32 prim, GeometryPlan &geom,
                 const u8 *device_image = nullptr) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  EOT_CPU_ZONE("ExecuteDraw");
  PerfScope perf_scope(s.perf.draw_ms);
  s.perf.draws++;
  u64 lap_t0 = PerfNow();
  auto lap = [&](f64 &acc) {
    const u64 t1 = PerfNow();
    acc += static_cast<f64>(t1 - lap_t0) * PerfMsPerTick();
    lap_t0 = t1;
  };
  DeviceView dev = Device(device_va);
  if (device_image) {
    dev.snapshot = device_image;
    dev.snapshotSize = kDeviceSnapshotBytes;
  } else if (const u8 *live = mem::at<u8>(device_va)) {
    dev.snapshot = live;
    dev.snapshotSize = dev::kDeviceSize;
  }
  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  u64 masks[5];
  for (u32 i = 0; i < 5; ++i) {
    if (device_image)
      masks[i] = (static_cast<u64>(dev.U32(8 * i)) << 32) | dev.U32(8 * i + 4);
    else
      masks[i] = s.pending_mask_valid ? s.pending_mask[i] : ~0ull;
    if (!masks[i])
      s.perf.mask_clean[i]++;
  }
  s.pending_mask_valid = false;
  const bool state_clean = !(masks[2] | masks[3] | masks[4]);
  if (state_clean)
    s.perf.mask_state_clean++;

  const u32 vs_va = dev.U32(dev::kVertexShader);
  const u32 ps_va = dev.U32(dev::kPixelShader);
  GuestShader *vs = FindGuestShader(s, vs_va);
  if (!vs && vs_va)
    vs = RegisterGuestShader(s, vs_va, false);
  GuestShader *ps = FindGuestShader(s, ps_va);
  if (!ps && ps_va)
    ps = RegisterGuestShader(s, ps_va, true);
  if (!vs) {
    u32 n;
    if (DiagShouldLog(0x6021, &n))
      EOT_WARN("[draw] no VS: vs_va={:#x} ps_va={:#x} prim={} {} n={} rt0={:#x} decl={:#x} "
               "stream0={:#x} frame={}",
               vs_va, ps_va, prim, geom.indexed ? "idx" : "vtx", geom.vertexCount,
               dev.U32(dev::kRenderTarget0), dev.U32(dev::kVertexDeclaration),
               dev.U32(dev::kStreamObject0), s.guest_frames);
    Dropped(vs_va ? "vertex shader object unreadable" : "no vertex shader bound", 0x6001);
    return;
  }
  if (ps_va && !ps) {
    Dropped("pixel shader object unreadable", 0x6013);
    return;
  }
  if (!vs->entry || (ps && !ps->entry)) {
    trace::Bump(trace::Counter::ShaderMiss);
    ResolveHostShader(s, *vs, 0);
    if (ps)
      ResolveHostShader(s, *ps, 0);
    Dropped("shader cache miss", 0x6002);
    return;
  }

  Targets targets;
  if (!ResolveTargets(s, dev, targets)) {
    Dropped("no render target or depth surface bound", 0x6003);
    return;
  }

  const InputLayout *layout = GetInputLayout(s, *vs, dev.U32(dev::kVertexDeclaration));
  if (!layout) {
    Dropped("no input layout for the bound declaration", 0x6004);
    return;
  }
  lap(s.perf.setup_ms);
  StreamInfo streams[16];
  for (u32 S = 0; S < 16; ++S) {
    if (!(layout->streamMask & (1u << S)))
      continue;
    if (S == 0 && geom.stream0OverrideVa) {
      streams[0].stream = 0;
      streams[0].stride = geom.stream0OverrideStride;
      streams[0].data = mem::at<u8>(geom.stream0OverrideVa);
      streams[0].sizeBytes = geom.vertexCount * geom.stream0OverrideStride;
      if (!streams[0].data || !streams[0].stride) {
        Dropped("BeginVertices data unreadable", 0x6015);
        return;
      }
      continue;
    }
    if (!ReadStream(dev, S, streams[S])) {
      Dropped("stream source unreadable", 0x6005);
      return;
    }
    if (layout->streamExtent[S] > streams[S].stride) {
      u32 n;
      if (DiagShouldLog(0x6006 + S, &n))
        EOT_WARN("[draw] stream {} stride {} smaller than the declaration extent {}", S,
                 streams[S].stride, layout->streamExtent[S]);
    }
  }

  PipelineState st;
  ZeroPipelineState(st);
  u32 spec = layout->spec;
  bool a2c = false;
  FillPipelineState(dev, targets, st, &spec, &a2c);
  spec |= layout->spec;
  st.vs = ResolveHostShader(s, *vs, spec);
  st.ps = ps ? ResolveHostShader(s, *ps, spec) : nullptr;
  if (!st.vs || (ps && !st.ps)) {
    Dropped("host shader unavailable (link failure)", 0x6007);
    return;
  }
  st.layout = layout;
  st.vsHash = vs->hash;
  st.psHash = ps ? ps->hash : 0;
  if (!targets.colorCount && targets.depth) {
    struct Snap {
      u32 mode, cache204, cache208, vtx, vte, win;
      float fs, fo, bs, bo, xs, xo, ys, yo;
    };
    static Snap last{};
    static u32 lines = 0;
    const Snap now{dev.U32(dev::kModeControl) & 0x1800u, mem::load<u32>(0x82496F7Cu + 204u * 4u),
                   mem::load<u32>(0x82496F7Cu + 208u * 4u), dev.U32(dev::kVtxControl),
                   dev.U32(dev::kVteControl), dev.U32(dev::kWindowOffset),
                   dev.F32(dev::kPolyOffsetFrontScale), dev.F32(dev::kPolyOffsetFrontOffset),
                   dev.F32(dev::kPolyOffsetBackScale), dev.F32(dev::kPolyOffsetBackOffset),
                   dev.F32(dev::kVportXScale), dev.F32(dev::kVportXOffset),
                   dev.F32(dev::kVportYScale), dev.F32(dev::kVportYOffset)};
    if (std::memcmp(&now, &last, sizeof(now)) != 0 && lines < 64) {
      last = now;
      ++lines;
      EOT_INFO("[shadow] caster draw: mode bits {:#x} front scale {:g} offset {:g} back scale {:g} "
               "offset {:g} | cache 204 {:g} 208 {:g} -> host bias {} slope {:g} (vs {:016x}) | "
               "vport x {:g}/{:g} y {:g}/{:g} vte {:#x} vtx {:#x} win {:#x} scale {:.4f}",
               now.mode, now.fs, now.fo, now.bs, now.bo, std::bit_cast<float>(now.cache204),
               std::bit_cast<float>(now.cache208), st.depthBias,
               st.slopeScaledDepthBias * st.targetScale, st.vsHash, now.xs, now.xo, now.ys,
               now.yo, now.vte, now.vtx, now.win, targets.scale);
    }
  }
  st.layoutKey = layout->key;
  st.spec = spec;
  for (u32 S = 0; S < 16; ++S)
    st.strides[S] = streams[S].stride;
  st.topology = geom.topology;
  if (geom.rectList)
    st.cull = plume::RenderCullMode::NONE;
  CanonicalizePipelineState(st,
                            (vs->entry ? vs->entry->specConstantsMask : 0u) |
                                (ps && ps->entry ? ps->entry->specConstantsMask : 0u),
                            layout->streamMask);
  s.current_vs_va = vs_va;
  s.current_ps_va = ps_va;
  {
    static const char *const kOrigins[2][3] = {{"stream/none", "stream/stream", "stream/bundle"},
                                               {"bundle/none", "bundle/stream", "bundle/bundle"}};
    s.current_origin =
        kOrigins[vs->createdByGuestCall ? 1 : 0][ps ? (ps->createdByGuestCall ? 2 : 1) : 0];
  }
  static PipelineState last_state{};
  static plume::RenderPipeline *last_pipeline = nullptr;
  plume::RenderPipeline *pipeline = last_pipeline;
  if (!pipeline || std::memcmp(reinterpret_cast<const u8 *>(&st) + kPipelineKeyOffset,
                               reinterpret_cast<const u8 *>(&last_state) + kPipelineKeyOffset,
                               sizeof(st) - kPipelineKeyOffset) != 0) {
    pipeline = GetOrCreatePipeline(s, st);
    if (pipeline) {
      last_state = st;
      last_pipeline = pipeline;
    }
  }
  lap(s.perf.pso_lookup_ms);
  if (!pipeline) {
    Dropped("pipeline creation failed", 0x6008);
    return;
  }
  struct LastDraw {
    bool valid = false;
    GuestShader *vs = nullptr, *ps = nullptr;
    const GuestSurface *color0 = nullptr, *depth = nullptr;
    const InputLayout *layout = nullptr;
    u32 prim = 0;
    plume::RenderPipeline *pipeline = nullptr;
    u32 bindings[80] = {};
  };
  static LastDraw last;
  const bool reusable = state_clean && last.valid && !geom.rectList && last.vs == vs &&
                        last.ps == ps &&
                        last.color0 == (targets.colorCount ? targets.color[0] : nullptr) &&
                        last.depth == targets.depth && last.layout == layout && last.prim == prim;
  if (reusable) {
    s.perf.mask_reusable++;
    if (last.pipeline != pipeline)
      s.perf.mask_pipeline_mismatch++;
  }

  UploadAlloc index_alloc;
  u32 index_count = 0;
  i32 host_base_vertex = 0;
  u32 lo = 0, hi = 0;
  RectExpansion rect;
  if (geom.rectList) {
    std::vector<u32> guest_vertices;
    if (geom.indexed) {
      guest_vertices = geom.indices;
      for (auto &v : guest_vertices)
        v = static_cast<u32>(static_cast<i32>(v) + geom.baseVertex);
    } else {
      guest_vertices.resize(geom.vertexCount);
      for (u32 i = 0; i < geom.vertexCount; ++i)
        guest_vertices[i] = geom.startVertex + i;
    }
    if (!ExpandRectList(*layout, streams, layout->streamMask, guest_vertices, rect) ||
        rect.indices.empty()) {
      Dropped("rect list expansion failed", 0x6009);
      return;
    }
    if (!UploadBytes(rect.indices.data(), rect.indices.size() * 4, 4, &index_alloc)) {
      Dropped("upload ring exhausted (indices)", 0x600A);
      return;
    }
    index_count = static_cast<u32>(rect.indices.size());
  } else if (geom.cached) {
    lo = static_cast<u32>(static_cast<i32>(geom.cached->lo) + geom.baseVertex);
    hi = static_cast<u32>(static_cast<i32>(geom.cached->hi) + geom.baseVertex);
    index_count = geom.cached->count;
    host_base_vertex = geom.baseVertex - static_cast<i32>(lo);
  } else if (geom.indexed) {
    if (geom.indices.empty()) {
      Dropped("empty index list", 0x600B);
      return;
    }
    lo = 0xFFFFFFFFu;
    hi = 0;
    for (u32 v : geom.indices) {
      const u32 gv = static_cast<u32>(static_cast<i32>(v) + geom.baseVertex);
      lo = std::min(lo, gv);
      hi = std::max(hi, gv);
    }
    if (!UploadBytes(geom.indices.data(), geom.indices.size() * 4, 4, &index_alloc)) {
      Dropped("upload ring exhausted (indices)", 0x600A);
      return;
    }
    index_count = static_cast<u32>(geom.indices.size());
    s.perf.index_bytes += u64(index_count) * 4;
    host_base_vertex = geom.baseVertex - static_cast<i32>(lo);
  } else {
    lo = geom.startVertex;
    hi = geom.startVertex + geom.vertexCount - 1;
  }

  plume::RenderVertexBufferView views[16];
  plume::RenderInputSlot slots[16];
  u32 max_slot = 0;
  for (u32 S = 0; S < 16; ++S)
    if (layout->streamMask & (1u << S))
      max_slot = S;
  UploadAlloc zero;
  if (!UploadZeroBuffer(s, &zero)) {
    Dropped("upload ring exhausted (zero buffer)", 0x600C);
    return;
  }
  for (u32 S = 0; S <= max_slot; ++S) {
    slots[S] = plume::RenderInputSlot(S, streams[S].stride);
    if (!(layout->streamMask & (1u << S))) {
      views[S] = plume::RenderVertexBufferView(plume::RenderBufferReference(zero.buffer, zero.offset),
                                               4096);
      slots[S] = plume::RenderInputSlot(S, 0);
      continue;
    }
    const StreamInfo &st_info = streams[S];
    UploadAlloc va;
    if (geom.rectList) {
      if (!UploadBytes(rect.vertices[S].data(), rect.vertices[S].size(), 16, &va)) {
        Dropped("upload ring exhausted (vertices)", 0x600D);
        return;
      }
      views[S] = plume::RenderVertexBufferView(plume::RenderBufferReference(va.buffer, va.offset),
                                               static_cast<u32>(va.size));
      continue;
    }
    const u64 first = u64(lo) * st_info.stride;
    u64 bytes = (u64(hi) - lo + 1) * st_info.stride;
    if (st_info.sizeBytes && first + bytes > st_info.sizeBytes) {
      if (first >= st_info.sizeBytes) {
        Dropped("vertex range outside the stream", 0x600E);
        return;
      }
      bytes = st_info.sizeBytes - first;
    }
    if (bytes > 64ull * 1024 * 1024) {
      Dropped("vertex range implausibly large", 0x600F);
      return;
    }
    if (const VertexMirror *mirror = GetVertexMirror(s, st_info, first, bytes)) {
      views[S] = plume::RenderVertexBufferView(
          plume::RenderBufferReference(mirror->buffer, mirror->offset),
          static_cast<u32>(bytes));
      continue;
    }
    if (!UploadAllocate(bytes, 16, &va)) {
      Dropped("upload ring exhausted (vertices)", 0x600D);
      return;
    }
    {
      PerfScope copy_scope(s.perf.vertex_copy_ms);
      CopyVertexBytes(va.cpu, st_info.data + first, static_cast<u32>(bytes));
    }
    s.perf.vertex_bytes += bytes;
    views[S] = plume::RenderVertexBufferView(plume::RenderBufferReference(va.buffer, va.offset),
                                             static_cast<u32>(bytes));
  }

  lap(s.perf.stream_ms);

  UploadAlloc vs_consts, ps_consts, shared_alloc;
  if (!UploadFloatFile(s, dev, dev::kVsFloatConstants, 0, vs->floatConstantRegs, &vs_consts) ||
      !UploadFloatFile(s, dev, dev::kPsFloatConstants, 1, ps ? ps->floatConstantRegs : 16u,
                       &ps_consts)) {
    Dropped("constant upload failed", 0x6010);
    return;
  }
  s.perf.constant_bytes += sizeof(SharedConstants);
  const ViewportInfo vp = ComputeViewport(dev, targets);
  SharedConstants sc;
  {
    PerfScope bind_scope(s.perf.bind_ms);
    BindTexturesAndSamplers(s, dev, sc);
  }
  static_assert(offsetof(SharedConstants, booleans) == sizeof(last.bindings));
  if (reusable && std::memcmp(last.bindings, &sc, sizeof(last.bindings)) != 0)
    s.perf.mask_fetch_mismatch++;
  last.valid = true;
  last.vs = vs;
  last.ps = ps;
  last.color0 = targets.colorCount ? targets.color[0] : nullptr;
  last.depth = targets.depth;
  last.layout = layout;
  last.prim = prim;
  last.pipeline = pipeline;
  std::memcpy(last.bindings, &sc, sizeof(last.bindings));
  for (u32 i = 0; i < 4; ++i) {
    sc.booleans[i] = dev.U32(dev::kVsBoolConstants + 4 * i);
    sc.booleans[4 + i] = dev.U32(dev::kPsBoolConstants + 4 * i);
  }
  FillLoopConstants(dev, dev::kVsLoopConstants, &sc.loopConstants[0]);
  FillLoopConstants(dev, dev::kPsLoopConstants, &sc.loopConstants[16]);
  sc.swappedTexcoords = layout->swappedTexcoords;
  sc.swappedNormals = layout->swappedNormals;
  sc.swappedBinormals = layout->swappedBinormals;
  sc.swappedTangents = layout->swappedTangents;
  sc.swappedBlendWeights = layout->swappedBlendWeights;
  sc.swappedPositions = layout->swappedPositions;
  sc.sintTexcoords |= layout->sintTexcoords;
  sc.packedDec3 = layout->packedDec3;
  if (targets.colorCount) {
    const u32 f0 = targets.color[0]->colorFormat;
    if (f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA))
      sc.packedDec3 |= 1u << 31;
    if (f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8) ||
        f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) ||
        f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_2_10_10_10) ||
        f0 == static_cast<u32>(xe::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10))
      sc.packedDec3 |= 1u << 30;
  }
  sc.alphaThreshold = dev.F32(dev::kAlphaRef);
  std::memcpy(sc.posScale, vp.posScale, sizeof(sc.posScale));
  std::memcpy(sc.posOffset, vp.posOffset, sizeof(sc.posOffset));
  if (!UploadBytes(&sc, sizeof(sc), kConstantBufferAlignment, &shared_alloc)) {
    Dropped("shared constant upload failed", 0x6011);
    return;
  }
  lap(s.perf.const_ms);

  if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) {
    static u32 k = 0;
    const u32 vte = dev.U32(dev::kVteControl);
    EOT_INFO("[diag] draw {} prim {} {} n={} pso={} key={:016x} mode={:#x} dc={:#x} srm={:#x} srmbf={:#x} rt0={:#x} {}x{} fmt{} rtexp{} ds={:#x} vs={:016x} ps={:016x} "
             "vp=({:.0f},{:.0f} {:.0f}x{:.0f} z{:.2f}-{:.2f}) vte={:#x} xs={:.1f} ys={:.1f} zs={:.3f} "
             "zo={:.3f} scis=({},{},{},{}) cull={} z={}{} func{} blend={} mask={:#x} spec={:#x} "
             "streams={:#x} stride0={} tex0={:#x} posScale=({:.3f},{:.3f},{:.3f}) "
             "posOff=({:.3f},{:.3f},{:.3f})",
             k++, prim, geom.indexed ? "idx" : "vtx",
             geom.indexed ? index_count : geom.vertexCount, static_cast<const void *>(pipeline),
             HashPipelineState(st), dev.U32(dev::kModeControl), dev.U32(dev::kDepthControl),
             dev.U32(dev::kStencilRefMask), dev.U32(dev::kStencilRefMaskBF),
             targets.colorCount ? targets.color[0]->va : 0, targets.width, targets.height,
             targets.colorCount ? targets.color[0]->colorFormat : 99,
             targets.colorCount ? targets.color[0]->colorExpBias : 0,
             targets.depth ? targets.depth->va : 0, vs->hash, ps ? ps->hash : 0, vp.vp.x, vp.vp.y,
             vp.vp.width, vp.vp.height, vp.vp.minDepth, vp.vp.maxDepth, vte,
             dev.F32(dev::kVportXScale), dev.F32(dev::kVportYScale), dev.F32(dev::kVportZScale),
             dev.F32(dev::kVportZOffset), vp.scissor.left, vp.scissor.top, vp.scissor.right,
             vp.scissor.bottom, static_cast<u32>(st.cull), st.depthEnable ? "on" : "off",
             st.depthWrite ? "w" : "", static_cast<u32>(st.depthFunc),
             st.blend[0].blendEnabled, st.blend[0].renderTargetWriteMask, spec,
             layout->streamMask, streams[0].stride, dev.U32(dev::kTextureObject0),
             vp.posScale[0], vp.posScale[1], vp.posScale[2], vp.posOffset[0], vp.posOffset[1],
             vp.posOffset[2]);
    for (u32 slot = 0; slot < 16; ++slot) {
      const u32 tex_va = dev.U32(dev::kTextureObject0 + 4 * slot);
      if (!tex_va)
        continue;
      u32 fc[6];
      for (u32 d = 0; d < 6; ++d)
        fc[d] = dev.U32(dev::kFetchConstants + 24 * slot + 4 * d);
      GuestTexture *gt = GetGuestTexture(s, tex_va);
      EOT_INFO("[diag]   tex{} va={:#x} fc=[{:08x} {:08x} {:08x} {:08x} {:08x} {:08x}] fmt={} "
               "dim={} {}x{}x{} {} {} {} biased={:#x} base={:#x} pitch={}",
               slot, tex_va, fc[0], fc[1], fc[2], fc[3], fc[4], fc[5],
               gt ? static_cast<u32>(gt->format) : 0xFFFFu,
               gt ? static_cast<u32>(gt->dimension) : 9u, gt ? gt->width : 0u,
               gt ? gt->height : 0u, gt ? gt->depth : 0u, gt && gt->tiled ? "tiled" : "linear",
               gt && gt->gammaSigned ? "gamma" : "lin",
               gt && gt->resolveOwned ? "resolve" : "upload", sc.biasedTextures,
               gt ? gt->baseAddress : 0u, ((fc[0] >> 22) & 0x1FF) * 32);
      static u32 dumped = 0;
      if (gt && !gt->tiled && gt->format == xe::TextureFormat::k_8 && dumped < 8) {
        const u32 pitch = ((fc[0] >> 22) & 0x1FF) * 32;
        const u8 *src = mem::phys<const u8>(gt->baseAddress);
        if (src && pitch >= gt->width) {
          const std::string path =
              std::format("logs/f{}_tex{}_{:x}_{}x{}_p{}.bin", s.guest_frames + 1, slot, tex_va,
                          gt->width, gt->height, pitch);
          if (FILE *f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(src, 1, static_cast<size_t>(pitch) * gt->height, f);
            std::fclose(f);
            ++dumped;
            EOT_INFO("[diag]   -> wrote {}", path);
          }
        }
      }
    }
  }

  if (!BindTargets(s, targets)) {
    Dropped("framebuffer creation failed", 0x6012);
    return;
  }
  GpuTimingCountDraw(s);
  auto *cmd = s.command_list;
  const bool pipeline_changed = s.bound_pipeline != pipeline;
  if (pipeline_changed) {
    cmd->setPipeline(pipeline);
    s.bound_pipeline = pipeline;
    if (st.stencilEnable) {
#if defined(EOT_D3D12)
      static_cast<plume::D3D12CommandList *>(cmd)->d3d->OMSetStencilRef(st.stencilRef);
#else
      vkCmdSetStencilReference(static_cast<plume::VulkanCommandList *>(cmd)->vk,
                               VK_STENCIL_FACE_FRONT_AND_BACK, st.stencilRef);
#endif
    }
  }
  static plume::RenderViewport last_vp;
  static plume::RenderRect last_sc;
  if (pipeline_changed || std::memcmp(&last_vp, &vp.vp, sizeof(last_vp)) != 0) {
    cmd->setViewports(&vp.vp, 1);
    last_vp = vp.vp;
  }
  if (pipeline_changed || std::memcmp(&last_sc, &vp.scissor, sizeof(last_sc)) != 0) {
    cmd->setScissors(&vp.scissor, 1);
    last_sc = vp.scissor;
  }
  const UploadAlloc *roots[3] = {&vs_consts, &ps_consts, &shared_alloc};
  for (u32 r = 0; r < 3; ++r) {
    if (s.bound_root_buffer[r] == roots[r]->buffer && s.bound_root_offset[r] == roots[r]->offset)
      continue;
    cmd->setGraphicsRootDescriptor(plume::RenderBufferReference(roots[r]->buffer, roots[r]->offset), r);
    s.bound_root_buffer[r] = roots[r]->buffer;
    s.bound_root_offset[r] = roots[r]->offset;
  }
  cmd->setVertexBuffers(0, views, max_slot + 1, slots);
  if (layout->needsSyntheticSlot) {
    plume::RenderVertexBufferView zv(plume::RenderBufferReference(zero.buffer, zero.offset), 4096);
    plume::RenderInputSlot zs(kSyntheticVertexSlot, 0);
    cmd->setVertexBuffers(kSyntheticVertexSlot, &zv, 1, &zs);
  }
  if (geom.rectList) {
    plume::RenderIndexBufferView ib(plume::RenderBufferReference(index_alloc.buffer, index_alloc.offset),
                                    index_count * 4, plume::RenderFormat::R32_UINT);
    cmd->setIndexBuffer(&ib);
    cmd->drawIndexedInstanced(index_count, 1, 0, 0, 0);
  } else if (geom.cached) {
    const u32 elem = geom.cached->is32 ? 4 : 2;
    plume::RenderIndexBufferView ib(
        plume::RenderBufferReference(geom.cached->buffer, geom.cached->offset), index_count * elem,
        geom.cached->is32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
    cmd->setIndexBuffer(&ib);
    cmd->drawIndexedInstanced(index_count, 1, 0, host_base_vertex, 0);
  } else if (geom.indexed) {
    plume::RenderIndexBufferView ib(plume::RenderBufferReference(index_alloc.buffer, index_alloc.offset),
                                    index_count * 4, plume::RenderFormat::R32_UINT);
    cmd->setIndexBuffer(&ib);
    cmd->drawIndexedInstanced(index_count, 1, 0, host_base_vertex, 0);
  } else {
    cmd->drawInstanced(geom.vertexCount, 1, 0, 0);
  }
  lap(s.perf.record_ms);
  (void)prim;
  DrainHostDebugMessages(s, "draw");
}

}

void DrawGuestPrimitives(u32 device_va, u32 prim, u32 start_vertex, u32 vertex_count) {
  if (!vertex_count)
    return;
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(prim, &expand);
  g.startVertex = start_vertex;
  g.vertexCount = vertex_count;
  if (prim == kPrimRectList) {
    g.rectList = true;
  } else if (expand) {
    g.indices.resize(vertex_count);
    for (u32 i = 0; i < vertex_count; ++i)
      g.indices[i] = start_vertex + i;
    ExpandIndices(prim, g.indices, false);
    g.indexed = true;
    g.baseVertex = 0;
    g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  ExecuteDraw(device_va, prim, g);
}

void QueueGuestUpDraw(u32 device_va, u32 prim, u32 vertex_count, u32 stride, u32 data_va) {
  FlushPendingUpDraw();
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.pending_up.valid = data_va != 0 && vertex_count != 0;
  s.pending_up_armed.store(s.pending_up.valid, std::memory_order_release);
  s.pending_up.device_va = device_va;
  s.pending_up.primitive_type = prim;
  s.pending_up.vertex_count = vertex_count;
  s.pending_up.stride = stride;
  s.pending_up.data_va = data_va;
  s.pending_up.has_image = false;
  if (const u8 *image = mem::at<u8>(device_va)) {
    s.pending_up.device_image.assign(image, image + kDeviceSnapshotBytes);
    s.pending_up.has_image = true;
  }
}

void FlushPendingUpDraw() {
  auto &s = state();
  if (!s.pending_up_armed.load(std::memory_order_acquire))
    return;
  VideoState::PendingUpDraw up;
  {
    std::lock_guard lock(s.mutex);
    if (!s.pending_up.valid)
      return;
    up = s.pending_up;
    s.pending_up.valid = false;
    s.pending_up_armed.store(false, std::memory_order_relaxed);
  }
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(up.primitive_type, &expand);
  g.startVertex = 0;
  g.vertexCount = up.vertex_count;
  g.stream0OverrideVa = up.data_va;
  g.stream0OverrideStride = up.stride;
  if (up.primitive_type == kPrimRectList) {
    g.rectList = true;
  } else if (expand) {
    g.indices.resize(up.vertex_count);
    for (u32 i = 0; i < up.vertex_count; ++i)
      g.indices[i] = i;
    ExpandIndices(up.primitive_type, g.indices, false);
    g.indexed = true;
    g.topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  }
  trace::Bump(trace::Counter::DrawVertices);
  ExecuteDraw(up.device_va, up.primitive_type, g,
              up.has_image ? up.device_image.data() : nullptr);
}

void DrawGuestIndexedPrimitives(u32 device_va, u32 prim, i32 base_vertex, u32 start_index,
                                u32 index_count) {
  if (!index_count)
    return;
  DeviceView dev = Device(device_va);
  GeometryPlan g;
  bool expand = false;
  g.topology = ConvertPrimitiveType(prim, &expand);
  g.indexed = true;
  g.baseVertex = base_vertex;
  {
    PerfScope index_scope(state().perf.index_ms);
    if (const CachedIndexRange *cached =
            GetCachedIndexRange(state(), dev.U32(dev::kIndexBuffer), prim, start_index, index_count)) {
      g.cached = cached;
      g.topology = cached->topology;
    }
  }
  if (g.cached) {
    ExecuteDraw(device_va, prim, g);
    return;
  }
  {
    PerfScope index_scope(state().perf.index_ms);
    if (!ReadGuestIndices(dev.U32(dev::kIndexBuffer), start_index, index_count, g.indices)) {
      Dropped("index buffer unreadable", 0x6020);
      return;
    }
    if (prim == kPrimRectList) {
      g.rectList = true;
    } else if (expand || prim == kPrimTriangleStrip || prim == kPrimLineStrip) {
      ExpandIndices(prim, g.indices, true);
      g.topology = prim == kPrimLineStrip ? plume::RenderPrimitiveTopology::LINE_LIST
                                          : plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    }
  }
  ExecuteDraw(device_va, prim, g);
}

void ClearGuestTargets(u32 device_va, u32 flags, u32 rect_va, u32 color_va, float z, u32 stencil) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  DeviceView dev = Device(device_va);
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  Targets targets;
  if (!ResolveTargets(s, dev, targets) || !BindTargets(s, targets))
    return;
  plume::RenderColor color(0, 0, 0, 0);
  if (color_va) {
    color = plume::RenderColor(mem::f32at(color_va), mem::f32at(color_va + 4),
                               mem::f32at(color_va + 8), mem::f32at(color_va + 12));
  }
  plume::RenderRect rect;
  const plume::RenderRect *rects = nullptr;
  u32 rect_count = 0;
  if (rect_va) {
    const float k = targets.scale;
    rect = plume::RenderRect(ScalePxBy(mem::load<i32>(rect_va), k),
                             ScalePxBy(mem::load<i32>(rect_va + 4), k),
                             ScalePxBy(mem::load<i32>(rect_va + 8), k),
                             ScalePxBy(mem::load<i32>(rect_va + 12), k));
    rects = &rect;
    rect_count = 1;
  }
  auto *cmd = s.command_list;
  for (u32 i = 0; i < targets.colorCount; ++i) {
    if (flags & (1u << i))
      cmd->clearColor(i, color, rects, rect_count);
  }
  if (targets.depth && (flags & 0x30)) {
    cmd->clearDepthStencil((flags & 0x10) != 0, (flags & 0x20) != 0, z, stencil & 0xFF, rects,
                           rect_count);
  }
  DrainHostDebugMessages(s, "clear");
}

}
