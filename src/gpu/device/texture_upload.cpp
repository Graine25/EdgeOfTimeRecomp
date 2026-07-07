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

struct Stats {
  std::atomic<u32> queued{0};
  std::atomic<u32> uploaded{0};
  std::atomic<u32> skipped{0};
};
Stats g_stats;

struct HostFootprint {
  u32 pitch = 0;      // bytes, 256-aligned as D3D12 wants
  u32 rowTexels = 0;
  u32 rows = 0;
  u64 size() const { return u64(pitch) * rows; }
  bool valid() const { return pitch != 0 && rows != 0 && rowTexels != 0; }
};

HostFootprint FootprintFor(const GuestTexture &tex) {
  HostFootprint fp;
  const u32 bpt = BytesPerTexel(tex.format);
  fp.pitch = ComputeTexturePitch(&tex);
  if (!fp.pitch || !bpt || !tex.height)
    return {};
  fp.rows = tex.height;
  fp.rowTexels = fp.pitch / bpt;
  return fp;
}

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
    const HostFootprint fp = FootprintFor(*tex);
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
            fp.rowTexels));

    const plume::RenderTextureBarrier to_read(
        tex->texture, plume::RenderTextureLayout::SHADER_READ);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1);
    tex->layout = plume::RenderTextureLayout::SHADER_READ;

    keep_alive.push_back(std::move(staging));
    if (g_stats.uploaded.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_INFO("[texture] first upload: {}x{} fmt={} pitch={} rows={}",
               tex->width, tex->height, static_cast<u32>(tex->format), fp.pitch,
               fp.rows);
    }
  }
}

void LogTextureUploadStats() {
  EOT_INFO("[texture] {} uploads queued, {} performed, {} skipped",
           g_stats.queued.load(), g_stats.uploaded.load(),
           g_stats.skipped.load());
}

}
