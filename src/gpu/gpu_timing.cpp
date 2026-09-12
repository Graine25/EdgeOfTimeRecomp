#include "gpu/gpu_timing.h"

#include <algorithm>
#include <format>
#include <memory>
#include <vector>

#include <plume_render_interface.h>
#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#endif

#include "core/logging.h"
#include "gpu/device.h"

namespace eot::gpu {

namespace {

constexpr u32 kQueryCount = 1024;

struct SlotTiming {
  std::unique_ptr<plume::RenderQueryPool> pool;
  std::vector<u32> journal;
  u32 used = 0;
  bool pending = false;
};

SlotTiming g_slots[kNumFrames];
u32 g_active_slot = 0;
u32 g_cat = kGpuCatOther;
bool g_supported = true;
bool g_open = false;

void WriteMark(SlotTiming &st, plume::RenderCommandList *cmd, u32 closing) {
  if (st.used >= kQueryCount)
    return;
  cmd->writeTimestamp(st.pool.get(), st.used++);
  st.journal.push_back(closing);
}

bool ReadbackIsMappable(plume::RenderQueryPool *pool) {
#if defined(EOT_D3D12)
  auto *d3d_pool = static_cast<plume::D3D12QueryPool *>(pool);
  auto *readback = d3d_pool->readbackBuffer.get();
  if (!readback || !readback->map())
    return false;
  readback->unmap();
#else
  (void)pool;
#endif
  return true;
}

const char *FormatName(u32 host_format) {
  switch (host_format) {
  case 10:
    return "16f";
  case 11:
    return "16u";
  case 20:
    return "8";
  case 24:
    return "10.10.10.2";
  case 50:
    return "11.11.10";
  default:
    return nullptr;
  }
}

}

u32 GpuTargetCategory(bool full_frame, bool has_depth, u32 color_count, u32 color0_host_format,
                      u32 samples, bool additive) {
  if (color_count == 0)
    return kGpuCatShadow;
  return 0x1000u | (full_frame && additive ? 0x2000u : 0u) | (full_frame ? 0x800u : 0u) |
         (has_depth ? 0x400u : 0u) | (samples > 1 ? 0x200u : 0u) | (color0_host_format & 0x1FFu);
}

std::string GpuCategoryName(u32 cat) {
  switch (cat) {
  case kGpuCatOther:
    return "other";
  case kGpuCatShadow:
    return "shadow";
  case kGpuCatResolve:
    return "resolve";
  case kGpuCatPresent:
    return "present";
  case kGpuCatUpload:
    return "upload";
  case kGpuCatResolveHw:
    return "resolve-hw";
  case kGpuCatResolveDepth:
    return "resolve-depth";
  case kGpuCatBroadcast:
    return "broadcast";
  default:
    break;
  }
  const bool full = cat & 0x800u, depth = cat & 0x400u, ms = cat & 0x200u, add = cat & 0x2000u;
  const u32 fmt = cat & 0x1FFu;
  const char *name = FormatName(fmt);
  return std::format("{} {}{}{}{}", full ? "full" : "small",
                     name ? std::string(name) : std::format("fmt{}", fmt), depth ? "+z" : "",
                     add ? "+add" : "", full && !ms ? "@1x" : "");
}

void GpuTimingFrameBegin(VideoState &s, plume::RenderCommandList *cmd, u32 slot) {
  if (!g_supported || !s.device || !cmd)
    return;
  if (!s.device->getCapabilities().queryPools) {
    g_supported = false;
    return;
  }
  auto &st = g_slots[slot];
  if (!st.pool) {
    st.pool = s.device->createQueryPool(kQueryCount);
    if (!st.pool) {
      g_supported = false;
      return;
    }
  }
  st.pending = false;
  st.used = 0;
  st.journal.clear();
  st.journal.push_back(kGpuCatOther);
  cmd->resetQueryPool(st.pool.get(), 0, kQueryCount);
  cmd->writeTimestamp(st.pool.get(), st.used++);
  g_active_slot = slot;
  g_cat = kGpuCatOther;
  g_open = true;
}

void GpuTimingMark(VideoState &s, plume::RenderCommandList *cmd, u32 cat) {
  (void)s;
  if (!g_supported || !g_open || cat == g_cat || !cmd)
    return;
  WriteMark(g_slots[g_active_slot], cmd, g_cat);
  g_cat = cat;
}

void GpuTimingCountDraw(VideoState &s) {
  if (!g_supported || !g_open)
    return;
  s.perf.gpu_cats[g_cat].second++;
}

void GpuTimingFrameEnd(plume::RenderCommandList *cmd) {
  if (!g_supported || !g_open || !cmd)
    return;
  auto &st = g_slots[g_active_slot];
  WriteMark(st, cmd, g_cat);
  st.pending = st.journal.size() > 1;
  g_open = false;
}

void GpuTimingCollect(VideoState &s, u32 slot) {
  if (!g_supported)
    return;
  auto &st = g_slots[slot];
  if (!st.pending || !st.pool)
    return;
  st.pending = false;
  if (!ReadbackIsMappable(st.pool.get())) {
    EOT_ERROR("[gpu-timing] query readback could not be mapped (slot {}); timing disabled", slot);
    g_supported = false;
    return;
  }
  st.pool->queryResults();
  const u64 *r = st.pool->getResults();
  const size_t n = std::min<size_t>(st.journal.size(), st.pool->getCount());
  if (n < 2 || r[n - 1] <= r[0])
    return;
  for (size_t i = 1; i < n; ++i) {
    if (r[i] <= r[i - 1])
      continue;
    s.perf.gpu_cats[st.journal[i]].first += (r[i] - r[i - 1]) * 1e-6;
  }
  s.perf.gpu_ms += (r[n - 1] - r[0]) * 1e-6;
  s.perf.gpu_frames++;
}

std::string GpuTimingSummary(const PerfCounters &p) {
  if (!g_supported)
    return "n/a";
  if (p.gpu_frames == 0)
    return "0";
  const f64 n = static_cast<f64>(p.gpu_frames);
  std::vector<std::pair<u32, std::pair<f64, u32>>> cats(p.gpu_cats.begin(), p.gpu_cats.end());
  std::sort(cats.begin(), cats.end(),
            [](const auto &a, const auto &b) { return a.second.first > b.second.first; });
  std::string out = std::format("{:.2f} ms/frame:", p.gpu_ms / n);
  u32 shown = 0;
  for (const auto &[cat, v] : cats) {
    if (v.first / n < 0.02 && shown >= 4)
      break;
    out += std::format(" {} {:.2f}", GpuCategoryName(cat), v.first / n);
    if (v.second)
      out += std::format(" ({} draws)", static_cast<u32>(v.second / std::max(1u, p.frames)));
    if (++shown >= 10)
      break;
  }
  return out;
}

}
