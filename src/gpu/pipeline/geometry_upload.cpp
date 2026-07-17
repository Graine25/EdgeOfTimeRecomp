#include "gpu/pipeline/geometry_upload.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/pipeline/constant_buffers.h"
#include "gpu/pipeline/vertex_layout.h"

namespace eot::gpu {

namespace {

std::mutex g_cache_mutex;
std::map<std::pair<u32, u32>, plume::RenderBufferReference> g_frame_cache;
std::atomic<u32> g_cache_hits{0};

std::atomic<u32> g_uploads{0};
std::atomic<u32> g_no_stream{0};
std::atomic<u32> g_no_indices{0};
std::atomic<u32> g_too_large{0};
std::atomic<u32> g_window_space{0};
std::atomic<u32> g_expanded{0};
std::atomic<u32> g_rects{0};

constexpr u32 kMaxStreamBytesPerDraw = 8u * 1024 * 1024;

bool CopyIndices16(u8 *dst, u32 guest_va, u32 count) {
  auto *out = reinterpret_cast<u16 *>(dst);
  for (u32 i = 0; i < count; ++i) {
    const u32 word = mem::try_load<u16>(guest_va + i * 2);
    out[i] = static_cast<u16>(word);
  }
  return true;
}

std::vector<u32> BuildTriangleIndices(u32 prim, u32 count) {
  static std::atomic<u32> seen[16]{};
  if (prim < 16)
    seen[prim].fetch_add(1, std::memory_order_relaxed);
  static std::atomic<u32> ticks{0};
  if ((ticks.fetch_add(1, std::memory_order_relaxed) % 20000) == 19999) {
    std::string line;
    for (u32 i = 0; i < 16; ++i) {
      const u32 n = seen[i].load(std::memory_order_relaxed);
      if (n)
        line += std::to_string(i) + ":" + std::to_string(n) + " ";
    }
    EOT_INFO("[geometry] primitive types seen: {}", line);
  }

  std::vector<u32> idx;
  switch (prim) {
  case kPrimQuadList: {
    const u32 quads = count / 4;
    idx.reserve(size_t(quads) * 6);
    for (u32 q = 0; q < quads; ++q) {
      const u32 b = q * 4;
      idx.insert(idx.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
    break;
  }
  case kPrimRectList: {
    const u32 rects = count / 3;
    idx.reserve(size_t(rects) * 6);
    for (u32 r = 0; r < rects; ++r) {
      const u32 b = r * 4;
      idx.insert(idx.end(), {b, b + 1, b + 2, b + 1, b + 3, b + 2});
    }
    break;
  }
  case kPrimTriangleFan:
    if (count >= 3) {
      idx.reserve(size_t(count - 2) * 3);
      for (u32 i = 1; i + 1 < count; ++i)
        idx.insert(idx.end(), {0u, i, i + 1});
    }
    break;
  case kPrimTriangleStrip:
    if (count >= 3) {
      idx.reserve(size_t(count - 2) * 3);
      for (u32 i = 0; i + 2 < count; ++i) {
        if (i & 1)
          idx.insert(idx.end(), {i + 1, i, i + 2});
        else
          idx.insert(idx.end(), {i, i + 1, i + 2});
      }
    }
    break;
  default:
    break;
  }
  return idx;
}

bool AcquireRange(u32 guest_va, u32 bytes, bool swap16,
                  plume::RenderBufferReference &out, u8 **mapped = nullptr) {
  const std::pair<u32, u32> key{guest_va, bytes};
  {
    std::lock_guard lock(g_cache_mutex);
    auto it = g_frame_cache.find(key);
    if (it != g_frame_cache.end()) {
      g_cache_hits.fetch_add(1, std::memory_order_relaxed);
      out = it->second;
      if (mapped)
        *mapped = nullptr;
      return true;
    }
  }

  auto alloc = constants::Allocate(bytes);
  if (!alloc.valid())
    return false;
  const bool ok = swap16 ? CopyIndices16(alloc.memory, guest_va, bytes / 2)
                         : constants::CopyGuestSwapped32(alloc.memory, guest_va,
                                                         bytes);
  if (!ok)
    return false;

  std::lock_guard lock(g_cache_mutex);
  g_frame_cache.emplace(key, alloc.ref);
  out = alloc.ref;
  if (mapped)
    *mapped = alloc.memory;
  return true;
}

}

void ResetGeometryFrame() {
  std::lock_guard lock(g_cache_mutex);
  g_frame_cache.clear();
}

void ConvertWindowSpacePositions(u8 *data, u32 bytes, u32 stride, u32 posOffset,
                                 u32 width, u32 height) {
  if (!stride || !width || !height || posOffset + 8 > stride)
    return;

  float max_abs = 0.0f;
  for (u32 v = 0; v * stride + posOffset + 8 <= bytes; ++v) {
    float xy[2];
    std::memcpy(xy, data + v * stride + posOffset, 8);
    max_abs = std::max(max_abs, std::max(std::abs(xy[0]), std::abs(xy[1])));
  }
  if (max_abs <= 1.5f)
    return;

  const float half_w = float(width) * 0.5f;
  const float half_h = float(height) * 0.5f;
  for (u32 v = 0; v * stride + posOffset + 8 <= bytes; ++v) {
    float xy[2];
    u8 *at = data + v * stride + posOffset;
    std::memcpy(xy, at, 8);
    xy[0] = xy[0] / half_w - 1.0f;
    xy[1] = 1.0f - xy[1] / half_h;
    std::memcpy(at, xy, 8);
  }
}

void RestoreByteAttributeOrder(const InputLayout &layout, u32 stream, u8 *data,
                               u32 bytes, u32 stride) {
  if (!stride)
    return;
  for (u32 e = 0; e < layout.count; ++e) {
    const auto &el = layout.elements[e];
    if (el.stream != stream)
      continue;
    if (el.format != plume::RenderFormat::R8G8B8A8_UINT &&
        el.format != plume::RenderFormat::R8G8B8A8_UNORM)
      continue;
    if (el.offset + 4 > stride)
      continue;
    for (u32 at = el.offset; at + 4 <= bytes; at += stride) {
      std::swap(data[at + 0], data[at + 3]);
      std::swap(data[at + 1], data[at + 2]);
    }
  }
}

void ExtrapolateRectCorner(const InputLayout &layout, u32 stream, u32 stride,
                           const u8 *v0, const u8 *v1, const u8 *v2, u8 *v3) {
  std::memcpy(v3, v1, stride);

  for (u32 e = 0; e < layout.count; ++e) {
    const auto &el = layout.elements[e];
    if (el.stream != stream)
      continue;

    u32 components = 0;
    switch (el.format) {
    case plume::RenderFormat::R32_FLOAT:
      components = 1;
      break;
    case plume::RenderFormat::R32G32_FLOAT:
      components = 2;
      break;
    case plume::RenderFormat::R32G32B32_FLOAT:
      components = 3;
      break;
    case plume::RenderFormat::R32G32B32A32_FLOAT:
      components = 4;
      break;
    default:
      continue;
    }
    if (el.offset + components * 4u > stride)
      continue;

    for (u32 c = 0; c < components; ++c) {
      const u32 at = el.offset + c * 4;
      float a = 0, b = 0, d = 0;
      std::memcpy(&a, v0 + at, 4);
      std::memcpy(&b, v1 + at, 4);
      std::memcpy(&d, v2 + at, 4);
      const float derived = b + d - a;
      std::memcpy(v3 + at, &derived, 4);
    }
  }
}

bool UploadDrawGeometry(const InputLayout &layout, u32 firstVertex,
                        u32 vertexCount, bool indexed, u32 startIndex,
                        u32 indexCount, u32 baseVertexIndex, u32 primitiveType,
                        bool windowSpace, u32 targetWidth, u32 targetHeight,
                        DrawGeometry &out) {
  out = DrawGeometry{};
  if (!vertexCount && !indexed)
    return false;

  const bool rect_list =
      primitiveType == kPrimRectList && !indexed && vertexCount >= 3;

  u32 streams[kMaxStreamSources];
  u32 stream_count = 0;
  for (u32 i = 0; i < layout.count; ++i) {
    const u32 stream = layout.elements[i].stream;
    bool seen = false;
    for (u32 j = 0; j < stream_count; ++j)
      seen = seen || streams[j] == stream;
    if (!seen && stream_count < kMaxStreamSources)
      streams[stream_count++] = stream;
  }
  if (!stream_count)
    return false;

  u32 first_needed = firstVertex;
  u32 needed_count = vertexCount;
  std::vector<u32> guest_indices;
  if (indexed) {
    const auto ib = Video::BoundIndexBuffer();
    const u32 unit = ib.index32 ? 4u : 2u;
    const u32 ib_first = startIndex * unit;
    if (!ib.address || !indexCount || ib_first >= ib.size) {
      g_no_indices.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const u32 avail = (ib.size - ib_first) / unit;
    const u32 count = std::min(indexCount, avail);
    guest_indices.resize(count);
    u32 lo = ~0u, hi = 0;
    for (u32 k = 0; k < count; ++k) {
      const u32 at = ib.address + ib_first + k * unit;
      const u32 v = ib.index32 ? mem::try_load<u32>(at) : mem::try_load<u16>(at);
      const u32 eff = v + baseVertexIndex;
      guest_indices[k] = eff;
      lo = std::min(lo, eff);
      hi = std::max(hi, eff);
    }
    if (lo > hi)
      return false;
    first_needed = lo;
    needed_count = hi - lo + 1;
    for (u32 &v : guest_indices)
      v -= lo;
  }

  for (u32 i = 0; i < stream_count; ++i) {
    const auto info = Video::BoundStream(streams[i]);
    if (!info.address || !info.stride) {
      g_no_stream.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    const u32 first_byte = first_needed * info.stride;
    if (first_byte >= info.size) {
      g_no_stream.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    u32 bytes = info.size - first_byte;
    if (needed_count)
      bytes = std::min(bytes, needed_count * info.stride);
    if (bytes > kMaxStreamBytesPerDraw) {
      g_too_large.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    if (rect_list) {
      const u32 rects = vertexCount / 3;
      const u32 expanded = rects * 4 * info.stride;
      if (expanded > kMaxStreamBytesPerDraw) {
        g_too_large.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      auto alloc = constants::Allocate(expanded);
      if (!alloc.valid())
        return false;

      for (u32 r = 0; r < rects; ++r) {
        u8 *quad = alloc.memory + size_t(r) * 4 * info.stride;
        for (u32 k = 0; k < 3; ++k) {
          const u32 src = (first_needed + r * 3 + k) * info.stride;
          if (src + info.stride > info.size ||
              !constants::CopyGuestSwapped32(quad + size_t(k) * info.stride,
                                             info.address + src,
                                             info.stride)) {
            g_no_stream.fetch_add(1, std::memory_order_relaxed);
            return false;
          }
        }
        ExtrapolateRectCorner(layout, streams[i], info.stride, quad,
                              quad + info.stride, quad + 2u * info.stride,
                              quad + 3u * info.stride);
      }

      RestoreByteAttributeOrder(layout, streams[i], alloc.memory, expanded,
                                info.stride);

      if (windowSpace) {
        for (u32 e = 0; e < layout.count; ++e) {
          const auto &el = layout.elements[e];
          if (el.usage != VertexUsage::kPosition || el.stream != streams[i])
            continue;
          ConvertWindowSpacePositions(alloc.memory, expanded, info.stride,
                                      el.offset, targetWidth, targetHeight);
          g_window_space.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }

      g_rects.fetch_add(rects, std::memory_order_relaxed);
      out.vertexViews[i] = plume::RenderVertexBufferView(alloc.ref, expanded);
      out.vertexSlots[i] = plume::RenderInputSlot(
          streams[i], info.stride,
          plume::RenderInputSlotClassification::PER_VERTEX_DATA);
      continue;
    }

    plume::RenderBufferReference ref;
    u8 *fresh = nullptr;
    if (!AcquireRange(info.address + first_byte, bytes, false, ref,
                      &fresh))
      return false;

    if (fresh)
      RestoreByteAttributeOrder(layout, streams[i], fresh, bytes, info.stride);

    if (windowSpace && fresh) {
      for (u32 e = 0; e < layout.count; ++e) {
        const auto &el = layout.elements[e];
        if (el.usage != VertexUsage::kPosition || el.stream != streams[i])
          continue;
        ConvertWindowSpacePositions(fresh, bytes, info.stride, el.offset,
                                    targetWidth, targetHeight);
        g_window_space.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }

    out.vertexViews[i] = plume::RenderVertexBufferView(ref, bytes);
    out.vertexSlots[i] = plume::RenderInputSlot(
        streams[i], info.stride,
        plume::RenderInputSlotClassification::PER_VERTEX_DATA);
  }
  out.vertexBufferCount = stream_count;

  if (!indexed) {
    const std::vector<u32> tri = BuildTriangleIndices(primitiveType, vertexCount);
    if (!tri.empty()) {
      const u32 bytes = static_cast<u32>(tri.size() * sizeof(u32));
      auto alloc = constants::Allocate(bytes);
      if (!alloc.valid())
        return false;
      std::memcpy(alloc.memory, tri.data(), bytes);
      out.indexView = plume::RenderIndexBufferView(alloc.ref, bytes,
                                                   plume::RenderFormat::R32_UINT);
      out.hasIndices = true;
      out.indexCount = static_cast<u32>(tri.size());
      g_expanded.fetch_add(1, std::memory_order_relaxed);
    }
  }

  if (indexed) {
    const std::vector<u32> tri =
        BuildTriangleIndices(primitiveType, static_cast<u32>(guest_indices.size()));
    std::vector<u32> final_indices;
    if (tri.empty()) {
      final_indices = std::move(guest_indices);
    } else {
      final_indices.resize(tri.size());
      for (size_t k = 0; k < tri.size(); ++k)
        final_indices[k] = guest_indices[tri[k]];
      g_expanded.fetch_add(1, std::memory_order_relaxed);
    }
    const u32 bytes = static_cast<u32>(final_indices.size() * sizeof(u32));
    if (!bytes)
      return false;
    auto alloc = constants::Allocate(bytes);
    if (!alloc.valid())
      return false;
    std::memcpy(alloc.memory, final_indices.data(), bytes);
    out.indexView = plume::RenderIndexBufferView(alloc.ref, bytes,
                                                 plume::RenderFormat::R32_UINT);
    out.hasIndices = true;
    out.indexCount = static_cast<u32>(final_indices.size());
  }

  g_uploads.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void LogGeometryUploadStats() {
  EOT_INFO("[geometry] {} draws fed, {} range copies reused, {} topologies "
           "expanded ({} rectangles), {} window-space rewrites; dropped: {} no "
           "stream, {} no indices, {} oversized",
           g_uploads.load(), g_cache_hits.load(), g_expanded.load(),
           g_rects.load(), g_window_space.load(), g_no_stream.load(),
           g_no_indices.load(), g_too_large.load());
}

}
