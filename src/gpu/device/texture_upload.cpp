#include "gpu/device/texture_upload.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/guest/format.h"

namespace tu = rex::graphics::texture_util;

namespace eot::gpu {

namespace {

std::mutex g_upload_mutex;
std::unordered_set<GuestTexture *> g_pending;
GuestTexture *g_last_uploaded = nullptr;
GuestTexture *g_last_fullscreen = nullptr;
u32 g_fullscreen_w = 0;
u32 g_fullscreen_h = 0;

struct Stats {
  std::atomic<u32> queued{0};
  std::atomic<u32> uploaded{0};
  std::atomic<u32> skipped{0};
  std::atomic<u32> mip_locks{0};
};
Stats g_stats;

}

void QueueTextureUpload(GuestTexture *tex) {
  if (!tex || !tex->texture || !tex->mappedMemory)
    return;
  std::lock_guard lock(g_upload_mutex);
  if (g_pending.insert(tex).second)
    g_stats.queued.fetch_add(1, std::memory_order_relaxed);
}

void ForgetTextureUpload(GuestTexture *tex) {
  if (!tex)
    return;
  std::lock_guard lock(g_upload_mutex);
  g_pending.erase(tex);
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
                         std::vector<std::unique_ptr<plume::RenderBuffer>> &keep_alive) {
  auto *device = Video::HostDevice();
  if (!cmd || !device)
    return;

  std::vector<GuestTexture *> batch;
  {
    std::lock_guard lock(g_upload_mutex);
    if (g_pending.empty())
      return;
    batch.assign(g_pending.begin(), g_pending.end());
    g_pending.clear();
  }

  for (GuestTexture *tex : batch) {
    if (!tex->texture || !tex->mappedMemory) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const TextureFootprint fp = ComputeTextureFootprint(tex);
    if (!fp.valid()) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const auto *src = mem::try_at<const u8>(tex->mappedMemory);
    if (!src) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    auto staging =
        device->createBuffer(plume::RenderBufferDesc::UploadBuffer(fp.size()));
    if (!staging) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    void *mapped = staging->map();
    if (!mapped) {
      g_stats.skipped.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    std::memcpy(mapped, src, size_t(fp.size()));
    staging->unmap();

    const plume::RenderTextureBarrier to_copy(
        tex->texture, plume::RenderTextureLayout::COPY_DEST);
    cmd->barriers(plume::RenderBarrierStage::COPY, &to_copy, 1);
    tex->layout = plume::RenderTextureLayout::COPY_DEST;

    cmd->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(tex->texture, 0, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(
            staging.get(), tex->format, tex->width, tex->height, 1,
            (fp.pitch / fp.unitBytes) * fp.blockSize));

    const plume::RenderTextureBarrier to_read(
        tex->texture, plume::RenderTextureLayout::SHADER_READ);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1);
    tex->layout = plume::RenderTextureLayout::SHADER_READ;

    keep_alive.push_back(std::move(staging));
    tex->hasContent = true;
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
  EOT_INFO("[texture] {} uploads queued, {} performed, {} skipped; {} mip locks "
           "not uploaded",
           g_stats.queued.load(), g_stats.uploaded.load(),
           g_stats.skipped.load(), g_stats.mip_locks.load());
}

}
