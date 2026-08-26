#include <algorithm>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
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

void LogPerfLocked(VideoState &s) {
  const i32 every = Settings::PerfFrames();
  PerfCounters &p = s.perf;
  const auto now = std::chrono::steady_clock::now();
  if (p.last_present.time_since_epoch().count() != 0)
    p.frame_ms += std::chrono::duration<f64, std::milli>(now - p.last_present).count();
  p.last_present = now;
  p.frames++;
  if (every <= 0 || static_cast<i32>(p.frames) < every)
    return;
  const f64 n = static_cast<f64>(p.frames);
  EOT_INFO("[perf] {} frames, {:.2f} ms/frame wall | cpu ms/frame: draw {:.2f} ({} draws; "
           "vtxcopy {:.2f} idx {:.2f} bind {:.2f}) resolve {:.2f} ({}) upload {:.2f} ({}) link "
           "{:.2f} ({}) pso {:.2f} ({}) | guest d3d {:.2f} ({} calls) | present acquire {:.2f} "
           "submit {:.2f} fence {:.2f} | KB/frame vtx {} idx {} const {}",
           p.frames, p.frame_ms / n, p.draw_ms / n, p.draws / p.frames, p.vertex_copy_ms / n,
           p.index_ms / n, p.bind_ms / n, p.resolve_ms / n, p.resolves / p.frames, p.upload_ms / n,
           p.uploads, p.link_ms / n, p.links, p.pso_ms / n, p.psos, p.guest_d3d_ms / n,
           p.guest_d3d_calls / p.frames, p.acquire_ms / n, p.submit_ms / n, p.fence_ms / n,
           p.vertex_bytes / p.frames / 1024, p.index_bytes / p.frames / 1024,
           p.constant_bytes / p.frames / 1024);
  p = PerfCounters{};
  p.last_present = now;
}

}

void Video::Present(u32 front_buffer_texture_va) {
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
    plume::RenderTexture *back = s.swap_chain->getTexture(image);
    plume::RenderTextureBarrier to_rt(back, plume::RenderTextureLayout::COLOR_WRITE);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_rt, 1);
    u32 src_index = kInvalidDescriptorIndex;
    if (front) {
      TransitionLocked(s, front->host, plume::RenderTextureLayout::SHADER_READ);
      src_index = BindTextureSRVSwizzledLocked(s, front->host, (front->fetch[3] >> 1) & 0xFFF);
      front->lastUseFrame = s.guest_frames;
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
      const float src_w = static_cast<float>(std::max(1u, front->host.width));
      const float src_h = static_cast<float>(std::max(1u, front->host.height));
      const float scale = std::min(out_w / src_w, out_h / src_h);
      const float w = src_w * scale, h = src_h * scale;
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
    plume::RenderTextureBarrier to_present(back, plume::RenderTextureLayout::PRESENT);
    cmd->barriers(plume::RenderBarrierStage::NONE, &to_present, 1);

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
    drained_slot = s.recording_slot();
    DrainHostDebugMessages(s, "present");
    RenderDocFrameBoundary(s.guest_frames);
  }
  if (drained_slot != ~0u)
    DrainSlot(s, drained_slot);
}

}
