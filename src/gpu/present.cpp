#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "core/profiling.h"
#include "gpu/device.h"
#include "gpu/gpu_timing.h"
#include "gpu/imgui_overlay.h"
#include "gpu/patches/aspect_ratio.h"
#include "gpu/patches/movie_aspect.h"
#include "gpu/patches/present_effects.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/settings.h"
#include "gpu/surfaces.h"
#include "gpu/textures.h"
#include "gpu/trace.h"

namespace eot::gpu {

namespace {

bool HandleResize(VideoState &s) {
  if (!s.resize_requested.exchange(false, std::memory_order_acq_rel))
    return true;
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
  s.swap_framebuffers.clear();
  s.swap_chain->resize();
  if (s.swap_chain->isEmpty())
    return false;
  return BuildSwapFramebuffers(s);
}

u32 EnsureGammaLutLocked(VideoState &s) {
  if (s.gamma_mode == VideoState::GammaMode::None || !Settings::PresentGamma())
    return kInvalidDescriptorIndex;
  HostTexture &lut = s.gamma_lut;
  if (!lut.valid()) {
    plume::RenderTextureDesc desc;
    desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
    desc.width = 1024;
    desc.height = 1;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = plume::RenderFormat::R16G16B16A16_UNORM;
    desc.committed = true;
    lut.texture = CreateHostTexture(s.device.get(), desc, "gamma-lut");
    if (!lut.valid())
      return kInvalidDescriptorIndex;
    lut.format = desc.format;
    lut.viewFormat = plume::RenderFormat::UNKNOWN;
    lut.viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    lut.width = desc.width;
    lut.height = desc.height;
    lut.depth = 1;
    lut.mipLevels = 1;
    lut.arraySize = 1;
    lut.layout = plume::RenderTextureLayout::UNKNOWN;
    s.gamma_lut_dirty = true;
  }
  if (s.gamma_lut_dirty) {
    UploadAlloc staging;
    if (!UploadAllocate(1024 * 8, kTexturePlacementAlignment, &staging))
      return kInvalidDescriptorIndex;
    auto *dst = reinterpret_cast<uint16_t *>(staging.cpu);
    for (u32 v = 0; v < 1024; ++v) {
      for (u32 c = 0; c < 3; ++c) {
        u32 out;
        if (s.gamma_mode == VideoState::GammaMode::Pwl) {
          const u32 i = v >> 3, f = v & 7;
          out = u32(s.gamma_pwl[c][i][0]) + (u32(s.gamma_pwl[c][i][1]) * f) / 8;
        } else {
          const u32 i = v >> 2, f = v & 3;
          const i32 a = s.gamma_table[c][i];
          const i32 b = s.gamma_table[c][std::min(i + 1, 255u)];
          out = static_cast<u32>(std::max(0, a + ((b - a) * i32(f)) / 4));
        }
        dst[v * 4 + c] = static_cast<uint16_t>(std::min(out, 65535u));
      }
      dst[v * 4 + 3] = 0xFFFF;
    }
    EOT_INFO("[present] gamma LUT rebuilt ({}): r(0)={:.4f} r(32)={:.4f} r(128)={:.4f} "
             "r(512)={:.4f} r(1023)={:.4f}",
             s.gamma_mode == VideoState::GammaMode::Pwl ? "pwl" : "table",
             dst[0] / 65535.0f, dst[32 * 4] / 65535.0f, dst[128 * 4] / 65535.0f,
             dst[512 * 4] / 65535.0f, dst[1023 * 4] / 65535.0f);
    if (FILE *f = std::fopen("logs/gamma_lut.txt", "w")) {
      for (u32 v = 0; v < 1024; ++v)
        std::fprintf(f, "%u %u %u\n", dst[v * 4], dst[v * 4 + 1], dst[v * 4 + 2]);
      std::fclose(f);
    }
    TransitionLocked(s, lut, plume::RenderTextureLayout::COPY_DEST);
    s.command_list->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(lut.texture.get(), 0, 0),
        plume::RenderTextureCopyLocation::PlacedFootprint(staging.buffer, lut.format, 1024, 1, 1,
                                                          1024, staging.offset),
        0, 0, 0);
    s.gamma_lut_dirty = false;
  }
  TransitionLocked(s, lut, plume::RenderTextureLayout::SHADER_READ);
  return BindTextureSRVLocked(s, lut);
}

f64 g_pace_ms = 0.0;

void LogPerfLocked(VideoState &s) {
  const i32 every = Settings::PerfFrames();
  PerfCounters &p = s.perf;
  const auto now = std::chrono::steady_clock::now();
  if (p.last_present.time_since_epoch().count() != 0)
    p.frame_ms += std::chrono::duration<f64, std::milli>(now - p.last_present).count();
  p.last_present = now;
  p.frames++;

  const i32 hitch_ms = Settings::HitchMs();
  if (hitch_ms > 0 && p.last_present.time_since_epoch().count() != 0) {
    const PerfCounters &q = s.perf_prev_frame;
    const f64 wall = p.frame_ms - q.frame_ms;
    if (wall > static_cast<f64>(hitch_ms)) {
      EOT_WARN("[hitch] frame {} took {:.1f} ms: draw {:.2f} ({} draws) upload {:.2f} ({} tex) "
               "resolve {:.2f} ({}) link {:.2f} ({}) pso {:.2f} ({}) guest d3d {:.2f} ({} calls) "
               "| acquire {:.2f} submit {:.2f} fence {:.2f} pace {:.2f} | new host objects: "
               "{} tex ({} surface {} mirror {} guest, {} recycled) {} views {} fb, {} parked | "
               "vtx {} KB idx {} KB "
               "const {} KB | live {} tex {} surf, pool {} | resolve: mirror {:.2f} fb {:.2f} bind "
               "{:.2f} | scans: alias {:.2f} msaa {:.2f} | "
               "unaccounted {:.2f}",
               s.guest_frames, wall, p.draw_ms - q.draw_ms, p.draws - q.draws,
               p.upload_ms - q.upload_ms, p.uploads - q.uploads, p.resolve_ms - q.resolve_ms,
               p.resolves - q.resolves, p.link_ms - q.link_ms, p.links - q.links,
               p.pso_ms - q.pso_ms, p.psos - q.psos, p.guest_d3d_ms - q.guest_d3d_ms,
               p.guest_d3d_calls - q.guest_d3d_calls, p.acquire_ms - q.acquire_ms,
               p.submit_ms - q.submit_ms, p.fence_ms - q.fence_ms, g_pace_ms - q.pace_ms,
               p.host_textures - q.host_textures, p.host_tex_surface - q.host_tex_surface,
               p.host_tex_mirror - q.host_tex_mirror, p.host_tex_guest - q.host_tex_guest,
               p.host_tex_recycled - q.host_tex_recycled,
               p.host_views - q.host_views,
               p.host_framebuffers - q.host_framebuffers, p.host_parked - q.host_parked,
               (p.vertex_bytes - q.vertex_bytes) / 1024, (p.index_bytes - q.index_bytes) / 1024,
               (p.constant_bytes - q.constant_bytes) / 1024, p.live_textures,
               p.live_surfaces, p.pool_size, p.resolve_mirror_ms - q.resolve_mirror_ms,
               p.resolve_fb_ms - q.resolve_fb_ms, p.resolve_bind_ms - q.resolve_bind_ms,
               p.alias_scan_ms - q.alias_scan_ms, p.msaa_scan_ms - q.msaa_scan_ms,
               wall - (p.draw_ms - q.draw_ms) - (p.upload_ms - q.upload_ms) -
                   (p.resolve_ms - q.resolve_ms) - (p.guest_d3d_ms - q.guest_d3d_ms) -
                   (p.acquire_ms - q.acquire_ms) - (p.submit_ms - q.submit_ms) -
                   (p.fence_ms - q.fence_ms) - (g_pace_ms - q.pace_ms));
    }
  }
  EvictStaleGuestSurfaces(s);
  {
    const PerfCounters &prev = s.perf_prev_frame;
    EOT_PLOT("host textures created", p.host_textures - prev.host_textures);
    EOT_PLOT("host views created", p.host_views - prev.host_views);
    EOT_PLOT("host framebuffers created", p.host_framebuffers - prev.host_framebuffers);
    EOT_PLOT("live guest textures", s.textures.size());
    EOT_PLOT("live surfaces", s.surfaces.size());
    EOT_PLOT("draws", p.draws - prev.draws);
    EOT_PLOT("resolves", p.resolves - prev.resolves);
  }
  EvictStaleGuestTextures(s);
  EvictHostTexturePool(s);
  p.live_textures = static_cast<u32>(s.textures.size());
  if (every > 0 && static_cast<i32>(p.frames) >= every) {
    u32 buckets[6] = {};
    u64 texels = 0, resolve_owned = 0, uploaded = 0;
    for (const auto &kv : s.textures) {
      const GuestTexture &t = *kv.second;
      if (t.resolveOwned) {
        resolve_owned++;
        continue;
      }
      if (!t.uploaded)
        continue;
      uploaded++;
      const u32 dim = std::max(t.width, t.height);
      buckets[dim >= 2048 ? 0 : dim >= 1024 ? 1 : dim >= 512 ? 2 : dim >= 256 ? 3 : dim >= 128 ? 4 : 5]++;
      texels += u64(t.width) * t.height;
    }
    EOT_INFO("[texstats] {} guest textures uploaded ({} resolve-owned): >=2048 {} | 1024 {} | 512 {} | "
             "256 {} | 128 {} | smaller {} | {:.1f} Mtexels base level",
             uploaded, resolve_owned, buckets[0], buckets[1], buckets[2], buckets[3], buckets[4],
             buckets[5], texels / 1e6);
  }
  p.live_surfaces = static_cast<u32>(s.surfaces.size());
  s.perf_prev_frame = p;
  s.perf_prev_frame.pace_ms = g_pace_ms;

  if (every <= 0 || static_cast<i32>(p.frames) < every)
    return;
  const f64 n = static_cast<f64>(p.frames);
  EOT_INFO("[perf] {} frames, {:.2f} ms/frame wall | cpu ms/frame: draw {:.2f} ({} draws; "
           "setup {:.2f} psolk {:.2f} streams {:.2f} [vtxcopy {:.2f}] const {:.2f} [bind {:.2f}, {} file hits] rec {:.2f}; idx {:.2f} outside) resolve {:.2f} ({}; {} copies, {} hw, {} dead) upload {:.2f} ({}) link "
           "{:.2f} ({}) pso {:.2f} ({}) | guest d3d {:.2f} ({} calls) | idxcache hit {} miss {} vtxcache hit {} miss {} "
           "| present acquire {:.2f} submit {:.2f} fence {:.2f} pace {:.2f} | KB/frame vtx {} "
           "idx {} const {} | gpu {}",
           p.frames, p.frame_ms / n, p.draw_ms / n, p.draws / p.frames, p.setup_ms / n,
           p.pso_lookup_ms / n, p.stream_ms / n, p.vertex_copy_ms / n, p.const_ms / n,
           p.bind_ms / n, p.const_file_hits / p.frames, p.record_ms / n, p.index_ms / n, p.resolve_ms / n, p.resolves / p.frames, p.resolve_copies / p.frames, p.resolve_hw / p.frames, p.dead_resolves / p.frames, p.upload_ms / n,
           p.uploads, p.link_ms / n, p.links, p.pso_ms / n, p.psos, p.guest_d3d_ms / n,
           p.guest_d3d_calls / p.frames, p.index_cache_hits / p.frames, p.index_cache_misses,
           p.vertex_cache_hits / p.frames, p.vertex_cache_misses,
           p.acquire_ms / n, p.submit_ms / n, p.fence_ms / n, g_pace_ms / n,
           p.vertex_bytes / p.frames / 1024,
           p.index_bytes / p.frames / 1024, p.constant_bytes / p.frames / 1024,
           GpuTimingSummary(p));
  p = PerfCounters{};
  p.last_present = now;
  g_pace_ms = 0.0;
}

void SleepUntil(std::chrono::steady_clock::time_point when) {
  using clock = std::chrono::steady_clock;
  constexpr auto kSpinTail = std::chrono::microseconds(400);
  const auto now = clock::now();
  if (when <= now)
    return;
#if defined(_WIN32)
  static HANDLE timer = []() -> HANDLE {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return h ? h : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }();
  if (timer && when - now > kSpinTail) {
    LARGE_INTEGER due;
    due.QuadPart = -(std::chrono::duration_cast<std::chrono::nanoseconds>(when - now - kSpinTail)
                         .count() /
                     100);
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
      WaitForSingleObject(timer, INFINITE);
  }
#else
  if (when - now > kSpinTail)
    std::this_thread::sleep_until(when - kSpinTail);
#endif
  while (clock::now() < when)
    std::this_thread::yield();
}

void FrameLimitWait() {
  using clock = std::chrono::steady_clock;
  static clock::time_point deadline{};
  static i32 cadence_fps = 0;
  const i32 fps = Settings::FpsLimit();
  if (fps <= 0) {
    deadline = clock::time_point{};
    cadence_fps = 0;
    return;
  }
  const auto period =
      std::chrono::duration_cast<clock::duration>(std::chrono::duration<f64>(1.0 / fps));
  const auto now = clock::now();
  if (deadline == clock::time_point{} || cadence_fps != fps) {
    cadence_fps = fps;
    deadline = now + period;
  } else {
    deadline += period;
    if (now > deadline) {
      deadline = now;
      return;
    }
  }
  const auto before = clock::now();
  SleepUntil(deadline);
  g_pace_ms += std::chrono::duration<f64, std::milli>(clock::now() - before).count();
}

}

void Video::Present(u32 front_buffer_texture_va) {
  EOT_CPU_ZONE("Present");
  auto &s = state();
  u32 drained_slot = ~0u;
  {
    std::lock_guard lock(s.mutex);
    s.guest_frames++;
    trace::EndFrame(s.guest_frames);
    if (!s.ready || s.shutting_down.load(std::memory_order_acquire))
      return;
    s.last_front_buffer_va = front_buffer_texture_va;
    BeginCommandList(s);
    if (!s.command_list_open)
      return;

    GuestTexture *front = nullptr;
    if (front_buffer_texture_va) {
      front = GetGuestTexture(s, front_buffer_texture_va);
      if (front && !front->host.valid())
        front = nullptr;
    }
    const bool want_vsync = Settings::Vsync();
    const bool vsync_changed = s.swap_chain->isVsyncEnabled() != want_vsync;
    s.swap_chain->setVsyncEnabled(want_vsync);
#if !defined(EOT_D3D12)
    if (vsync_changed)
      s.resize_requested.store(true, std::memory_order_release);
#else
    (void)vsync_changed;
#endif
    if (!HandleResize(s)) {
      SubmitOpenListLocked(s);
      AdvanceAndWaitReused(s);
      return;
    }

    const u32 cur = s.recording_slot();
    u32 image = 0;
    bool acquired = false;
    {
      PerfScope perf_scope(s.perf.acquire_ms);
      acquired = s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(), &image) &&
                 image < s.swap_framebuffers.size();
    }
    if (!acquired) {
      u32 n;
      if (DiagShouldLog(0x8001, &n))
        EOT_WARN("[present] acquireTexture failed (minimised?)");
      SubmitOpenListLocked(s);
      AdvanceAndWaitReused(s);
      return;
    }

    auto *cmd = s.command_list;
    GpuTimingMark(s, cmd, kGpuCatPresent);
    plume::RenderTexture *back = s.swap_chain->getTexture(image);
    plume::RenderTextureBarrier to_rt(back, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_rt, 1);
    u32 src_index = kInvalidDescriptorIndex;
    if (front) {
      TransitionLocked(s, front->host, plume::RenderTextureLayout::SHADER_READ);
      src_index = BindTextureSRVSwizzledLocked(s, front->host, (front->fetch[3] >> 1) & 0xFFF);
      front->lastUseFrame = s.guest_frames;
      src_index = ApplyPresentEffects(s, front->host, src_index);
      GpuTimingMark(s, cmd, kGpuCatPresent);
    }
    const u32 lut_index = front ? EnsureGammaLutLocked(s) : kInvalidDescriptorIndex;
    cmd->setFramebuffer(s.swap_framebuffers[image].get());
    s.bound_framebuffer = nullptr;
    cmd->clearColor(0, plume::RenderColor(0, 0, 0, 1), nullptr, 0);
    const i32 dump_every = Settings::DumpEvery();
    if (front && dump_every > 0 && ((s.presented_frames + 1) % static_cast<u64>(dump_every)) == 0) {
      const std::string path = std::format("logs/frame_{}.ppm", s.presented_frames + 1);
      DumpHostTextureLocked(s, front->host, path.c_str(), 1.0f, lut_index,
                            (front->fetch[3] >> 1) & 0xFFF);
      cmd->setFramebuffer(s.swap_framebuffers[image].get());
    }
    if (src_index != kInvalidDescriptorIndex) {
      const float out_w = static_cast<float>(s.swap_chain->getWidth());
      const float out_h = static_cast<float>(s.swap_chain->getHeight());
      ApplyAspectRatio();
      const bool movie = TakeMovieDrawnFlag();
      const float aspect =
          movie ? 16.0f / 9.0f : std::clamp(ConfiguredAspectRatio(), 0.5f, 4.5f);
      float w = out_w, h = out_w / aspect;
      if (h > out_h) {
        h = out_h;
        w = out_h * aspect;
      }
      const float x = (out_w - w) * 0.5f, y = (out_h - h) * 0.5f;
      plume::RenderViewport vp(x, y, w, h, 0.0f, 1.0f);
      plume::RenderRect sc(static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(x + w),
                           static_cast<i32>(y + h));
      cmd->setViewports(&vp, 1);
      cmd->setScissors(&sc, 1);
      plume::RenderPipeline *pso = GetBlitPipeline(s, plume::RenderFormat::B8G8R8A8_UNORM);
      cmd->setPipeline(pso);
      s.bound_pipeline = nullptr;
      CopyPushConstants pc;
      pc.resourceDescriptorIndex = src_index;
      pc.resourceDescriptorIndex2 = lut_index != kInvalidDescriptorIndex ? lut_index : 0u;
      pc.param0 = 1.0f;
      SelectPresentBlitMode(front->host.width, front->host.height, w, h, pc.extra);
      pc.colorAdjust[0] = static_cast<float>(std::clamp(Settings::Brightness(), -0.5, 0.5));
      pc.colorAdjust[1] = static_cast<float>(std::clamp(Settings::Contrast(), 0.25, 3.0));
      pc.colorAdjust[2] = static_cast<float>(std::clamp(Settings::Saturation(), 0.0, 3.0));
      pc.colorAdjust[3] = static_cast<float>(std::clamp(Settings::Gamma(), 0.4, 2.5));
      pc.param1 = lut_index != kInvalidDescriptorIndex ? 2.0f : 1.0f;
      pc.rect[0] = 0.0f;
      pc.rect[1] = 0.0f;
      pc.rect[2] = 1.0f;
      pc.rect[3] = 1.0f;
      cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc, kCopyPushConstantByteOffset,
                                    sizeof(pc));
      cmd->drawInstanced(3, 1, 0, 0);
    } else {
      u32 n;
      if (DiagShouldLog(0x8002, &n))
        EOT_WARN("[present] no front buffer mirror for {:#x}; presenting black",
                 front_buffer_texture_va);
    }
    {
      const u32 out_w = s.swap_chain->getWidth(), out_h = s.swap_chain->getHeight();
      const plume::RenderViewport full(0.0f, 0.0f, static_cast<float>(out_w),
                                       static_cast<float>(out_h), 0.0f, 1.0f);
      const plume::RenderRect full_scissor(0, 0, static_cast<i32>(out_w), static_cast<i32>(out_h));
      cmd->setViewports(&full, 1);
      cmd->setScissors(&full_scissor, 1);
      RunOverlayDrawHook(cmd, s.swap_framebuffers[image].get(), out_w, out_h);
      s.bound_pipeline = nullptr;
      s.bound_framebuffer = nullptr;
    }

    plume::RenderTextureBarrier to_present(back, plume::RenderTextureLayout::PRESENT);
    cmd->barriers(plume::RenderBarrierStage::NONE, &to_present, 1);

    GpuTimingFrameEnd(cmd);
    s.command_lists[cur]->end();
    s.command_list_open = false;
    s.bound_framebuffer = nullptr;
    s.bound_pipeline = nullptr;
    const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
    plume::RenderCommandSemaphore *wait[] = {s.acquire_semaphores[cur].get()};
    plume::RenderCommandSemaphore *signal[] = {s.render_semaphores[image].get()};
    {
      PerfScope perf_scope(s.perf.submit_ms);
      s.queue->executeCommandLists(lists, 1, wait, 1, signal, 1, s.fences[cur].get());
      s.command_list_submitted[cur] = true;
      s.swap_chain->present(image, signal, 1);
    }
    s.presented_frames++;
    trace::PresentMarker(s.presented_frames);

    {
      PerfScope perf_scope(s.perf.fence_ms);
      AdvanceAndWaitReused(s);
    }
    LogPerfLocked(s);
    PsoCacheFlushIfDirty(false);
    drained_slot = s.recording_slot();
    DrainHostDebugMessages(s, "present");
    RenderDocFrameBoundary(s.guest_frames);
  }
  if (drained_slot != ~0u)
    DrainSlot(s, drained_slot);
  FrameLimitWait();
  EOT_FRAME_MARK();
}

}
