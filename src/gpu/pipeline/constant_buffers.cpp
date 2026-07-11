#include "gpu/pipeline/constant_buffers.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/runtime.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/device/native_texture_mirror.h"
#include "gpu/guest/d3d.h"
#include "gpu/guest/texture_fetch.h"
#include "gpu/pipeline/vertex_layout.h"

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
std::atomic<u32> g_with_textures{0};
std::atomic<u32> g_slot_nonzero{0};
std::atomic<u32> g_resolve_failed{0};
std::atomic<u32> g_acquire_failed{0};

bool CreateChunk(UploadChunk &chunk) {
  auto *device = Video::HostDevice();
  if (!device)
    return false;
  chunk.buffer = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
      kUploadChunkSize, plume::RenderBufferFlag::CONSTANT |
                            plume::RenderBufferFlag::VERTEX |
                            plume::RenderBufferFlag::INDEX));
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

bool Is16BitComponentFormat(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R16G16_SINT:
  case plume::RenderFormat::R16G16_UINT:
  case plume::RenderFormat::R16G16_SNORM:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_SINT:
  case plume::RenderFormat::R16G16B16A16_UINT:
  case plume::RenderFormat::R16G16B16A16_SNORM:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
    return true;
  default:
    return false;
  }
}

void ApplySwapMasks(const InputLayout &layout, SharedConstants &shared) {
  for (u32 i = 0; i < layout.count; ++i) {
    const InputElement &e = layout.elements[i];
    if (!Is16BitComponentFormat(e.format))
      continue;
    const u32 bit = 1u << (e.usageIndex & 31u);
    switch (e.usage) {
    case VertexUsage::kPosition:
      shared.swappedPositions |= bit;
      break;
    case VertexUsage::kNormal:
      shared.swappedNormals |= bit;
      break;
    case VertexUsage::kBinormal:
      shared.swappedBinormals |= bit;
      break;
    case VertexUsage::kTangent:
      shared.swappedTangents |= bit;
      break;
    case VertexUsage::kBlendWeight:
      shared.swappedBlendWeights |= bit;
      break;
    case VertexUsage::kTexCoord:
      shared.swappedTexcoords |= bit;
      break;
    default:
      break;
    }
  }
}

void GatherSharedConstants(u32 device_va, SharedConstants &shared,
                           u32 &bound_textures) {
  CopyByteSwap32(reinterpret_cast<u8 *>(shared.booleansArr),
                 device_va + kVsBoolConstOffset, kBoolConstDwords * 4);
  CopyByteSwap32(reinterpret_cast<u8 *>(shared.booleansArr + kBoolConstDwords),
                 device_va + kPsBoolConstOffset, kBoolConstDwords * 4);

  const auto rt = Video::BoundAttachmentSize();
  if (rt.width && rt.height) {
    shared.halfPixelOffset[0] = -1.0f / static_cast<float>(rt.width);
    shared.halfPixelOffset[1] = 1.0f / static_cast<float>(rt.height);
  }

  shared.alphaThreshold =
      mem::try_load<float>(device_va + kAlphaRefOffset);

  for (u32 i = 0; i < kMaxSamplerSlots; ++i) {
    const u32 tex_va =
        mem::try_load<u32>(device_va + kTextureObjectShadow + i * 4);
    if (!tex_va)
      continue;
    g_slot_nonzero.fetch_add(1, std::memory_order_relaxed);
    GuestTexture *tex = ResolveGuestSurface(tex_va);
    if (!tex)
      tex = FindOrBuildNativeTexture(tex_va);
    if (!tex) {
      GuestTextureFetch fetch;
      if (DecodeTextureFetch(device_va, i, fetch))
        NoteTextureFetch(fetch);
      if (g_resolve_failed.fetch_add(1, std::memory_order_relaxed) == 0)
        EOT_WARN("[constants] sampler {}: texture object 0x{:08X} resolves to "
                 "no host record", i, tex_va);
      continue;
    }
    const u32 index = Video::AcquireTextureDescriptor(tex);
    if (index == kInvalidDescriptorIndex) {
      if (g_acquire_failed.fetch_add(1, std::memory_order_relaxed) == 0)
        EOT_WARN("[constants] sampler {}: texture 0x{:08X} ({}x{}) got no "
                 "descriptor (host tex {})",
                 i, tex_va, tex->width, tex->height, tex->texture != nullptr);
      continue;
    }
    shared.texture2DIndices[i] = index;
    shared.texture3DIndices[i] = index;
    shared.textureCubeIndices[i] = index;
    shared.texture1DIndices[i] = index;
    ++bound_textures;
  }

}

}

DrawConstants UploadDrawConstants(u32 device_va,
                                  const eot::gpu::InputLayout *layout) {
  DrawConstants out;
  if (!device_va) {
    g_failed.fetch_add(1, std::memory_order_relaxed);
    return out;
  }

  SharedConstants shared;
  u32 bound_textures = 0;
  GatherSharedConstants(device_va, shared, bound_textures);
  if (layout)
    ApplySwapMasks(*layout, shared);

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

  std::memcpy(out.shared.memory, &shared, sizeof(shared));

  if (bound_textures)
    g_with_textures.fetch_add(1, std::memory_order_relaxed);
  if ((g_uploads.fetch_add(1, std::memory_order_relaxed) + 1) % 200000 == 0)
    LogStats();
  return out;
}

Allocation Allocate(u32 size) {
  if (!size)
    return {};
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return AllocateLocked(s, size);
}

bool CopyGuestSwapped32(u8 *dst, u32 guest_va, u32 bytes) {
  return CopyByteSwap32(dst, guest_va, bytes);
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
  EOT_INFO("[constants] {} uploads ({} with a bound texture), {} failed, "
           "{} unreadable",
           g_uploads.load(), g_with_textures.load(), g_failed.load(),
           g_unreadable.load());
  EOT_INFO("[constants] sampler slots: {} non-zero, {} unresolved, {} no "
           "descriptor",
           g_slot_nonzero.load(), g_resolve_failed.load(),
           g_acquire_failed.load());
  LogTextureFetchCensus();
  LogNativeTextureStats();
}

}
