#include "gpu/pipeline/geometry_upload.h"

#include <algorithm>
#include <atomic>
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

constexpr u32 kMaxStreamBytesPerDraw = 8u * 1024 * 1024;

bool CopyIndices16(u8 *dst, u32 guest_va, u32 count) {
  auto *out = reinterpret_cast<u16 *>(dst);
  for (u32 i = 0; i < count; ++i) {
    const u32 word = mem::try_load<u16>(guest_va + i * 2);
    out[i] = static_cast<u16>(word);
  }
  return true;
}

bool AcquireRange(u32 guest_va, u32 bytes, bool swap16,
                  plume::RenderBufferReference &out) {
  const std::pair<u32, u32> key{guest_va, bytes};
  {
    std::lock_guard lock(g_cache_mutex);
    auto it = g_frame_cache.find(key);
    if (it != g_frame_cache.end()) {
      g_cache_hits.fetch_add(1, std::memory_order_relaxed);
      out = it->second;
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
  return true;
}

}

void ResetGeometryFrame() {
  std::lock_guard lock(g_cache_mutex);
  g_frame_cache.clear();
}

bool UploadDrawGeometry(const InputLayout &layout, u32 firstVertex,
                        u32 vertexCount, bool indexed, u32 startIndex,
                        u32 indexCount, DrawGeometry &out) {
  out = DrawGeometry{};
  if (!vertexCount && !indexed)
    return false;

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

  for (u32 i = 0; i < stream_count; ++i) {
    const auto info = Video::BoundStream(streams[i]);
    if (!info.address || !info.stride) {
      g_no_stream.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    const u32 first_byte = firstVertex * info.stride;
    if (first_byte >= info.size) {
      g_no_stream.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    u32 bytes = info.size - first_byte;
    if (!indexed && vertexCount)
      bytes = std::min(bytes, vertexCount * info.stride);
    if (bytes > kMaxStreamBytesPerDraw) {
      g_too_large.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    plume::RenderBufferReference ref;
    if (!AcquireRange(info.address + first_byte, bytes, false, ref))
      return false;

    out.vertexViews[i] = plume::RenderVertexBufferView(ref, bytes);
    out.vertexSlots[i] = plume::RenderInputSlot(
        streams[i], info.stride,
        plume::RenderInputSlotClassification::PER_VERTEX_DATA);
  }
  out.vertexBufferCount = stream_count;

  if (indexed) {
    const auto info = Video::BoundIndexBuffer();
    if (!info.address || !indexCount) {
      g_no_indices.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const u32 unit = info.index32 ? 4u : 2u;
    const u32 first_byte = startIndex * unit;
    if (first_byte >= info.size) {
      g_no_indices.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const u32 bytes =
        std::min(indexCount * unit, info.size - first_byte);
    plume::RenderBufferReference ref;
    if (!AcquireRange(info.address + first_byte, bytes, !info.index32, ref))
      return false;

    out.indexView = plume::RenderIndexBufferView(
        ref, bytes,
        info.index32 ? plume::RenderFormat::R32_UINT
                     : plume::RenderFormat::R16_UINT);
    out.hasIndices = true;
  }

  g_uploads.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void LogGeometryUploadStats() {
  EOT_INFO("[geometry] {} draws fed, {} range copies reused; dropped: {} no "
           "stream, {} no indices, {} oversized",
           g_uploads.load(), g_cache_hits.load(), g_no_stream.load(),
           g_no_indices.load(), g_too_large.load());
}

}
