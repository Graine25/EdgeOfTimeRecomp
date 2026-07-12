#include "gpu/device/texture_upload.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/settings.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/guest/format.h"

namespace tu = rex::graphics::texture_util;

REXCVAR_DEFINE_BOOL(eot_upload_textures, true, kCvarGroup, "Upload guest textures");

namespace eot::gpu {

namespace {

std::mutex g_upload_mutex;
std::unordered_set<GuestTexture *> g_dirty;

struct NativeUpload {
  GuestTexture *tex = nullptr;
  std::vector<u8> data;
  u32 rowPitch = 0;
  u32 rows = 0;
  u32 level = 0;
};
std::vector<NativeUpload> g_native_pending;
std::atomic<u32> g_native_uploaded{0};
GuestTexture *g_last_uploaded = nullptr;
GuestTexture *g_last_fullscreen = nullptr;
u32 g_fullscreen_w = 0;
u32 g_fullscreen_h = 0;

constexpr u32 kMaxEmptyUploadAttempts = 4;

constexpr u32 kGuestPageSize = 0x1000;

bool ReadableSpan(u32 va, u64 size) {
  if (!va || size == 0 || size > 0xFFFFFFFFull)
    return false;
  const u32 bytes = static_cast<u32>(size);
  if (va + bytes < va)
    return false;
  for (u32 p = va; p < va + bytes;
       p = (p & ~(kGuestPageSize - 1)) + kGuestPageSize) {
    if (!mem::try_translate(p, 1))
      return false;
  }
  return true;
}

struct Stats {
  std::atomic<u32> queued{0};
  std::atomic<u32> uploaded{0};
  std::atomic<u32> skipped{0};
  std::atomic<u32> mip_locks{0};
  std::atomic<u32> non_zero{0};
  std::atomic<u32> abandoned{0};
  std::atomic<u32> not_copyable{0};
};
Stats g_stats;

}

namespace {

plume::RenderBuffer *AcquireStaging(StagingPool &pool,
                                    plume::RenderDevice *device, u64 bytes) {
  constexpr u64 kGranularity = 64 * 1024;
  const u64 capacity = (bytes + kGranularity - 1) / kGranularity * kGranularity;

  for (u32 i = pool.used; i < pool.buffers.size(); ++i) {
    if (pool.capacities[i] < capacity)
      continue;
    if (i != pool.used) {
      std::swap(pool.buffers[i], pool.buffers[pool.used]);
      std::swap(pool.capacities[i], pool.capacities[pool.used]);
    }
    return pool.buffers[pool.used++].get();
  }

  auto buffer =
      device->createBuffer(plume::RenderBufferDesc::UploadBuffer(capacity));
  if (!buffer)
    return nullptr;
  plume::RenderBuffer *raw = buffer.get();
  pool.buffers.insert(pool.buffers.begin() + pool.used, std::move(buffer));
  pool.capacities.insert(pool.capacities.begin() + pool.used, capacity);
  ++pool.used;
  return raw;
}

}

void QueueTextureUpload(GuestTexture *tex) {
  if (!tex || !tex->texture || !tex->mappedMemory)
    return;
  std::lock_guard lock(g_upload_mutex);
  if (g_dirty.insert(tex).second)
    g_stats.queued.fetch_add(1, std::memory_order_relaxed);
}

void QueueNativeUpload(GuestTexture *tex, std::vector<u8> data, u32 rowPitch,
                       u32 rows, u32 level) {
  if (!tex || data.empty() || !rowPitch || !rows)
    return;
  std::lock_guard lock(g_upload_mutex);
  g_native_pending.push_back({tex, std::move(data), rowPitch, rows, level});
}

void ForgetTextureUpload(GuestTexture *tex) {
  if (!tex)
    return;
  std::lock_guard lock(g_upload_mutex);
  g_dirty.erase(tex);
  if (g_last_uploaded == tex)
    g_last_uploaded = nullptr;
  if (g_last_fullscreen == tex)
    g_last_fullscreen = nullptr;
}

GuestTexture *LastUploadedTexture(u32 preferred_w, u32 preferred_h) {
  std::lock_guard lock(g_upload_mutex);
  g_fullscreen_w = preferred_w;
  g_fullscreen_h = preferred_h;
  if (g_last_fullscreen && g_last_fullscreen->hasContent)
    return g_last_fullscreen;
  return g_last_uploaded;
}

void FlushTextureUploads(plume::RenderCommandList *cmd,
                         StagingPool &pool) {
  auto *device = Video::HostDevice();
  if (!cmd || !device || !REXCVAR_GET(eot_upload_textures))
    return;

  {
    std::vector<NativeUpload> native;
    {
      std::lock_guard lock(g_upload_mutex);
      native.swap(g_native_pending);
    }
    for (auto &up : native) {
      GuestTexture *tex = up.tex;
      if (!tex || !tex->texture)
        continue;
      const u64 bytes = u64(up.rowPitch) * up.rows;
      plume::RenderBuffer *staging = AcquireStaging(pool, device, bytes);
      if (!staging)
        continue;
      void *mapped = staging->map();
      if (!mapped)
        continue;
      std::memcpy(mapped, up.data.data(), size_t(bytes));
      staging->unmap();

      const plume::RenderTextureBarrier to_copy(
          tex->texture, plume::RenderTextureLayout::COPY_DEST);
      cmd->barriers(plume::RenderBarrierStage::COPY, &to_copy, 1);
      tex->layout = plume::RenderTextureLayout::COPY_DEST;

      const u32 block = IsBlockCompressed(tex->format) ? kTextureBlockSize : 1;
      const u32 unit = block == 1 ? BytesPerTexel(tex->format)
                                  : BytesPerBlock(tex->format);
      if (!unit)
        continue;
      const u32 level_w = std::max(tex->width >> up.level, 1u);
      const u32 level_h = std::max(tex->height >> up.level, 1u);
      cmd->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(tex->texture,
                                                         up.level, 0),
          plume::RenderTextureCopyLocation::PlacedFootprint(
              staging, tex->format, level_w, level_h, 1,
              (up.rowPitch / unit) * block));

      const plume::RenderTextureBarrier to_read(
          tex->texture, plume::RenderTextureLayout::SHADER_READ);
      cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1);
      tex->layout = plume::RenderTextureLayout::SHADER_READ;

      tex->hasContent = true;
      if (g_native_uploaded.fetch_add(1, std::memory_order_relaxed) == 0)
        EOT_INFO("[texture] first engine texture uploaded: {}x{} fmt={} "
                 "pitch={} rows={}",
                 tex->width, tex->height, static_cast<u32>(tex->format),
                 up.rowPitch, up.rows);
    }
  }

  std::vector<GuestTexture *> batch;
  {
    std::lock_guard lock(g_upload_mutex);
    if (g_dirty.empty())
      return;
    batch.assign(g_dirty.begin(), g_dirty.end());
    for (auto it = g_dirty.begin(); it != g_dirty.end();) {
      GuestTexture *tex = *it;
      if (tex->sawNonZeroSource) {
        it = g_dirty.erase(it);
      } else if (++tex->emptyUploadAttempts > kMaxEmptyUploadAttempts) {
        g_stats.abandoned.fetch_add(1, std::memory_order_relaxed);
        it = g_dirty.erase(it);
      } else {
        ++it;
      }
    }
  }

  for (GuestTexture *tex : batch) {
    if (!tex->texture || !tex->mappedMemory) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (IsDepthFormat(tex->format) ||
        tex->sampleCount != plume::RenderSampleCount::COUNT_1) {
      if (g_stats.not_copyable.fetch_add(1, std::memory_order_relaxed) == 0) {
        EOT_INFO("[texture] skipping upload to a {} texture ({}x{} fmt={})",
                 IsDepthFormat(tex->format) ? "depth" : "multisampled",
                 tex->width, tex->height, static_cast<u32>(tex->format));
      }
      continue;
    }
    const TextureFootprint fp = ComputeTextureFootprint(tex);
    if (!fp.valid()) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (!ReadableSpan(tex->mappedMemory, fp.size())) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const auto *src = mem::try_at<const u8>(tex->mappedMemory);
    if (!src) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    const size_t bytes = size_t(fp.size());
    bool non_zero = false;
    for (size_t i = 0; i < bytes; ++i) {
      if (src[i] != 0) {
        non_zero = true;
        break;
      }
    }
    if (!non_zero) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (!tex->sawNonZeroSource) {
      tex->sawNonZeroSource = true;
      if (g_stats.non_zero.fetch_add(1, std::memory_order_relaxed) == 0) {
        EOT_INFO("[texture] first NON-ZERO scratch: {}x{} fmt={} - the guest "
                 "does write through our LockRect pointer",
                 tex->width, tex->height, static_cast<u32>(tex->format));
      }
    }

    plume::RenderBuffer *staging = AcquireStaging(pool, device, bytes);
    if (!staging) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    void *mapped = staging->map();
    if (!mapped) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    std::memcpy(mapped, src, bytes);
    staging->unmap();

    const plume::RenderTextureBarrier to_copy(
        tex->texture, plume::RenderTextureLayout::COPY_DEST);
    cmd->barriers(plume::RenderBarrierStage::COPY, &to_copy, 1);
    tex->layout = plume::RenderTextureLayout::COPY_DEST;

    cmd->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(tex->texture, 0, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            staging, tex->format, tex->width, tex->height, 1,
            (fp.pitch / fp.unitBytes) * fp.blockSize));

    const plume::RenderTextureBarrier to_read(
        tex->texture, plume::RenderTextureLayout::SHADER_READ);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1);
    tex->layout = plume::RenderTextureLayout::SHADER_READ;

    tex->hasContent = tex->sawNonZeroSource;
    {
      std::lock_guard lock(g_upload_mutex);
      g_last_uploaded = tex;
      if (tex->width == g_fullscreen_w && tex->height == g_fullscreen_h)
        g_last_fullscreen = tex;
    }
    if (g_stats.uploaded.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_INFO("[texture] first upload: {}x{} fmt={} pitch={} rows={}",
               tex->width, tex->height, static_cast<u32>(tex->format), fp.pitch,
               fp.rows);
    }
  }
}

void NoteMipLockSkipped() {
  g_stats.mip_locks.fetch_add(1, std::memory_order_relaxed);
}

void LogTextureUploadStats() {
  EOT_INFO("[texture] {} tracked, {} uploads, {} skipped, {} mip locks skipped; "
           "{} ever had non-zero content, {} abandoned empty, {} not copyable",
           g_stats.queued.load(), g_stats.uploaded.load(),
           g_stats.skipped.load(), g_stats.mip_locks.load(),
           g_stats.non_zero.load(), g_stats.abandoned.load(),
           g_stats.not_copyable.load());
}

}
