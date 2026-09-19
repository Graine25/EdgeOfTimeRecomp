#include "gpu/constant_buffers.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "core/logging.h"
#include "gpu/device.h"
#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#endif

namespace eot::gpu {

namespace {

constexpr u64 kChunkSize = 32ull * 1024 * 1024;

struct Chunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *cpu = nullptr;
  u64 capacity = 0;
  u64 used = 0;
  u64 gpuVa = 0;
};

struct Ring {
  std::vector<Chunk> chunks[kNumFrames];
  u64 frame_bytes[kNumFrames] = {};
};

Ring &ring() {
  static Ring r;
  return r;
}

bool MakeChunk(Chunk &chunk, u64 size) {
  auto &s = state();
  plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(size);
  desc.flags = plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX |
               plume::RenderBufferFlag::CONSTANT;
  chunk.buffer = CreateHostBuffer(s.device.get(), desc, "upload-ring");
  if (!chunk.buffer)
    return false;
  chunk.cpu = static_cast<u8 *>(chunk.buffer->map());
  if (!chunk.cpu) {
    EOT_ERROR("upload ring: map() failed for a {} byte chunk", size);
    chunk.buffer.reset();
    return false;
  }
  chunk.capacity = size;
  chunk.used = 0;
#if defined(EOT_D3D12)
  chunk.gpuVa = static_cast<plume::D3D12Buffer *>(chunk.buffer.get())->d3d->GetGPUVirtualAddress();
#else
  chunk.gpuVa = chunk.buffer->getDeviceAddress();
  if (!chunk.gpuVa) {
    EOT_ERROR("upload ring: the device gave the chunk no buffer device address (VK_KHR_buffer_device_address)");
    chunk.buffer->unmap();
    chunk.buffer.reset();
    return false;
  }
#endif
  return true;
}

}

bool UploadRingInit() {
  auto &r = ring();
  for (u32 i = 0; i < kNumFrames; ++i) {
    r.chunks[i].clear();
    Chunk c;
    if (!MakeChunk(c, kChunkSize))
      return false;
    r.chunks[i].push_back(std::move(c));
  }
  return true;
}

static std::mutex g_ring_mutex;
static std::atomic<u64> g_ring_epoch{0};
u64 UploadRingEpoch() { return g_ring_epoch.load(std::memory_order_acquire); }

void UploadRingResetFrame(u32 slot) {
  std::lock_guard lock(g_ring_mutex);
  auto &r = ring();
  g_ring_epoch.fetch_add(1, std::memory_order_acq_rel);
  auto &chunks = r.chunks[slot];
  for (size_t i = 0; i < chunks.size(); ++i)
    chunks[i].used = 0;
  while (chunks.size() > 4) {
    chunks.back().buffer->unmap();
    chunks.pop_back();
  }
  r.frame_bytes[slot] = 0;
}

bool UploadAllocate(u64 size, u64 alignment, UploadAlloc *out) {
  *out = UploadAlloc{};
  if (size == 0)
    return false;
  auto &s = state();
  auto &r = ring();
  if (alignment == 0)
    alignment = 1;
  std::lock_guard lock(g_ring_mutex);
  const u32 slot = s.recording_slot();
  auto &chunks = r.chunks[slot];
  for (u32 attempt = 0; attempt < 2; ++attempt) {
    for (auto &c : chunks) {
      const u64 start = (c.used + alignment - 1) / alignment * alignment;
      if (start + size <= c.capacity) {
        out->buffer = c.buffer.get();
        out->offset = start;
        out->cpu = c.cpu + start;
        out->size = size;
        out->gpuVa = c.gpuVa ? c.gpuVa + start : 0;
        c.used = start + size;
        r.frame_bytes[slot] += size;
        return true;
      }
    }
    Chunk c;
    const u64 want = size + alignment > kChunkSize ? size + alignment : kChunkSize;
    if (!MakeChunk(c, want))
      return false;
    chunks.push_back(std::move(c));
  }
  return false;
}

bool UploadBytes(const void *src, u64 size, u64 alignment, UploadAlloc *out) {
  if (!UploadAllocate(size, alignment, out))
    return false;
  std::memcpy(out->cpu, src, size);
  return true;
}

u64 UploadRingBytesThisFrame() {
  auto &s = state();
  return ring().frame_bytes[s.recording_slot()];
}

}
