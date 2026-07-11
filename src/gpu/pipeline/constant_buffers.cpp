#include "gpu/pipeline/constant_buffers.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/runtime.h>

#include "core/logging.h"
#include "gpu/device/device.h"
#include "gpu/guest/d3d.h"

namespace eot::gpu::constants {

namespace {

constexpr u32 kUploadChunkSize = 8u * 1024 * 1024;

constexpr u32 kConstantAlignment = 256;

struct UploadChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *mapped = nullptr;
};

struct FrameArena {
  std::vector<UploadChunk> chunks;
  u32 chunkIndex = 0;
  u32 chunkOffset = 0;
  u32 peakChunks = 0;
};

struct UploadState {
  std::mutex mutex;
  FrameArena frames[kNumFrames];
  u32 cursor = 0;
};

UploadState &state() {
  static UploadState s;
  return s;
}

std::atomic<u32> g_uploads{0};
std::atomic<u32> g_failed{0};
std::atomic<u32> g_unreadable{0};

bool CreateChunk(UploadChunk &chunk) {
  auto *device = Video::HostDevice();
  if (!device)
    return false;
  chunk.buffer = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
      kUploadChunkSize, plume::RenderBufferFlag::CONSTANT));
  if (!chunk.buffer) {
    EOT_ERROR("[constants] createBuffer({} MiB) failed",
              kUploadChunkSize / (1024 * 1024));
    return false;
  }
  chunk.mapped = static_cast<u8 *>(chunk.buffer->map());
  if (!chunk.mapped) {
    EOT_ERROR("[constants] RenderBuffer::map() returned null");
    chunk.buffer.reset();
    return false;
  }
  return true;
}

Allocation AllocateLocked(UploadState &s, u32 size) {
  FrameArena &arena = s.frames[s.cursor];
  u32 off = (arena.chunkOffset + kConstantAlignment - 1) & ~(kConstantAlignment - 1);
  if (off + size > kUploadChunkSize) {
    ++arena.chunkIndex;
    off = 0;
  }
  if (arena.chunks.size() <= arena.chunkIndex)
    arena.chunks.resize(arena.chunkIndex + 1);

  UploadChunk &chunk = arena.chunks[arena.chunkIndex];
  if (!chunk.buffer && !CreateChunk(chunk))
    return {};
  if (arena.chunkIndex + 1 > arena.peakChunks) {
    arena.peakChunks = arena.chunkIndex + 1;
    EOT_INFO("[constants] slot {} grew to {} chunk(s), {} MiB", s.cursor,
             arena.peakChunks,
             arena.peakChunks * (kUploadChunkSize / (1024 * 1024)));
  }

  arena.chunkOffset = off + size;
  Allocation a;
  a.memory = chunk.mapped + off;
  a.ref = plume::RenderBufferReference(chunk.buffer.get(), off);
  a.size = size;
  return a;
}

bool CopyByteSwap32(u8 *dst, u32 guest_va, u32 bytes) {
  auto *memory = REX_KERNEL_MEMORY();
  const auto *src = memory->TranslateVirtual<const u32 *>(guest_va);
  if (!src)
    return false;
  auto *out = reinterpret_cast<u32 *>(dst);
  const u32 dwords = bytes / 4;
  for (u32 i = 0; i < dwords; ++i)
    out[i] = __builtin_bswap32(src[i]);
  return true;
}

}

DrawConstants UploadDrawConstants(u32 device_va) {
  DrawConstants out;
  if (!device_va) {
    g_failed.fetch_add(1, std::memory_order_relaxed);
    return out;
  }

  auto &s = state();
  std::lock_guard lock(s.mutex);

  out.vs = AllocateLocked(s, kVsFloatConstBytes);
  out.ps = AllocateLocked(s, kPsFloatConstBytes);
  out.shared = AllocateLocked(s, sizeof(SharedConstants));
  if (!out.valid()) {
    g_failed.fetch_add(1, std::memory_order_relaxed);
    return {};
  }

  if (!CopyByteSwap32(out.vs.memory, device_va + kVsFloatConstOffset,
                      kVsFloatConstBytes) ||
      !CopyByteSwap32(out.ps.memory, device_va + kPsFloatConstOffset,
                      kPsFloatConstBytes)) {
    if (g_unreadable.fetch_add(1, std::memory_order_relaxed) == 0)
      EOT_WARN("[constants] device 0x{:08X}: float constant file unreadable",
               device_va);
    return {};
  }

  SharedConstants shared;
  CopyByteSwap32(reinterpret_cast<u8 *>(shared.booleansArr),
                 device_va + kVsBoolConstOffset, kBoolConstDwords * 4);
  CopyByteSwap32(reinterpret_cast<u8 *>(shared.booleansArr + kBoolConstDwords),
                 device_va + kPsBoolConstOffset, kBoolConstDwords * 4);

  const auto rt = Video::BoundAttachmentSize();
  if (rt.width && rt.height) {
    shared.halfPixelOffset[0] = -1.0f / static_cast<float>(rt.width);
    shared.halfPixelOffset[1] = 1.0f / static_cast<float>(rt.height);
  }

  std::memcpy(out.shared.memory, &shared, sizeof(shared));

  if ((g_uploads.fetch_add(1, std::memory_order_relaxed) + 1) % 200000 == 0)
    LogStats();
  return out;
}

void ResetFrame(u32 slot) {
  if (slot >= kNumFrames)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.cursor = slot;
  FrameArena &arena = s.frames[slot];
  arena.chunkIndex = 0;
  arena.chunkOffset = 0;
}

void Shutdown() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  for (auto &arena : s.frames) {
    for (auto &chunk : arena.chunks) {
      if (chunk.buffer && chunk.mapped)
        chunk.buffer->unmap();
    }
    arena.chunks.clear();
    arena.chunkIndex = 0;
    arena.chunkOffset = 0;
  }
}

void LogStats() {
  EOT_INFO("[constants] {} uploads, {} failed, {} unreadable", g_uploads.load(),
           g_failed.load(), g_unreadable.load());
}

}
