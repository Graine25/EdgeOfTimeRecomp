#include <plume_render_interface.h>

#include <algorithm>

#include "core/logging.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/gpu_profiling.h"
#include "gpu/gpu_timing.h"

namespace eot::gpu {

void DestroyHostTexture(VideoState &s, HostTexture &host);

namespace {

bool SameTextureShape(const HostTexture &pooled, const HostTexture &want,
                      const plume::RenderTextureDesc &d) {
  const plume::RenderTextureDesc &p = pooled.desc;
  return p.dimension == d.dimension && p.width == d.width && p.height == d.height &&
         p.depth == d.depth && p.mipLevels == d.mipLevels && p.arraySize == d.arraySize &&
         p.format == d.format && p.flags == d.flags &&
         p.multisampling.sampleCount == d.multisampling.sampleCount &&
         pooled.viewFormat == want.viewFormat && pooled.viewDimension == want.viewDimension &&
         pooled.isDepth == want.isDepth;
}

constexpr u64 kHostTexturePoolFrames = 120;
constexpr size_t kHostTexturePoolMax = 192;

}

void BeginCommandList(VideoState &s) {
  if (s.command_list_open)
    return;
  if (s.shutting_down.load(std::memory_order_acquire) || !s.pipeline_layout)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  s.command_list = s.command_lists[cur].get();
  s.command_list->begin();
  if (!s.null_texture_barriers_submitted) {
    plume::RenderTextureBarrier barriers[kNullTextureDescriptorCount];
    for (u32 i = 0; i < kNullTextureDescriptorCount; ++i) {
      barriers[i] = plume::RenderTextureBarrier(s.null_textures[i].get(),
                                                plume::RenderTextureLayout::SHADER_READ);
    }
    s.command_list->barriers(plume::RenderBarrierStage::NONE, barriers,
                             kNullTextureDescriptorCount);
    s.null_texture_barriers_submitted = true;
  }
  s.command_list->setGraphicsPipelineLayout(s.pipeline_layout.get());
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 0);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 1);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 2);
  s.command_list->setGraphicsDescriptorSet(s.sampler_descriptor_set.get(), 3);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 4);
  s.command_list_open = true;
  GpuTimingFrameBegin(s, s.command_list, cur);
#if EOT_GPU_PROFILING
  SetGPUProfilerCommandList(static_cast<plume::D3D12CommandList *>(s.command_list)->d3d);
#endif
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  for (auto &stream : s.bound_vertex_streams)
    stream.valid = false;
  s.bound_index_stream.valid = false;
  for (u32 i = 0; i < 3; ++i) {
    s.bound_root_buffer[i] = nullptr;
    s.bound_root_offset[i] = 0;
  }
  s.shared_bound = false;
}

void SubmitOpenListLocked(VideoState &s) {
  if (!s.command_list_open)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  GpuTimingFrameEnd(s.command_list);
  s.command_lists[cur]->end();
  s.command_list_open = false;
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  for (auto &stream : s.bound_vertex_streams)
    stream.valid = false;
  s.bound_index_stream.valid = false;
  for (u32 i = 0; i < 3; ++i) {
    s.bound_root_buffer[i] = nullptr;
    s.bound_root_offset[i] = 0;
  }
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, s.fences[cur].get());
  s.command_list_submitted[cur] = true;
}

void AdvanceAndWaitReused(VideoState &s) {
  const u32 slot = s.next_frame;
  s.frame.store(slot, std::memory_order_relaxed);
  s.next_frame = (slot + 1) % kNumFrames;
  s.command_list = s.command_lists[slot].get();
  if (s.command_list_submitted[slot]) {
    s.queue->waitForCommandFence(s.fences[slot].get());
    s.command_list_submitted[slot] = false;
    GpuTimingCollect(s, slot);
#if EOT_GPU_PROFILING
    if (auto *ctx = GpuProfilerCtx()) {
      TracyD3D12NewFrame(ctx);
      TracyD3D12Collect(ctx);
    }
#endif
  }
  s.view_graveyard[slot].clear();
  s.framebuffer_graveyard[slot].clear();
  s.texture_graveyard[slot].clear();
  s.buffer_graveyard[slot].clear();
  DrainDescriptorSlotsLocked(s, slot);
  UploadRingResetFrame(slot);
}

void ParkTexture(VideoState &s, std::unique_ptr<plume::RenderTexture> t) {
  if (t)
    s.perf.host_parked++;
  if (t)
    s.texture_graveyard[s.recording_slot()].push_back(std::move(t));
}

void ParkView(VideoState &s, std::unique_ptr<plume::RenderTextureView> v) {
  if (v)
    s.view_graveyard[s.recording_slot()].push_back(std::move(v));
}

void ParkFramebuffer(VideoState &s, std::unique_ptr<plume::RenderFramebuffer> f) {
  if (f)
    s.framebuffer_graveyard[s.recording_slot()].push_back(std::move(f));
}

void ParkBuffer(VideoState &s, std::unique_ptr<plume::RenderBuffer> b) {
  if (b)
    s.buffer_graveyard[s.recording_slot()].push_back(std::move(b));
}

void ParkHostTexture(VideoState &s, HostTexture &host) {
  if (host.texture && s.host_texture_pool.size() < kHostTexturePoolMax) {
    VideoState::PooledHostTexture entry;
    entry.host = std::move(host);
    entry.freedFrame = s.guest_frames;
    s.host_texture_pool.push_back(std::move(entry));
    host = HostTexture{};
    return;
  }
  DestroyHostTexture(s, host);
}

void DestroyHostTexture(VideoState &s, HostTexture &host) {
  ReleaseTextureSRVLocked(s, host);
  if (host.texture) {
    const auto *dead = host.texture.get();
    for (auto it = s.framebuffers.begin(); it != s.framebuffers.end();) {
      bool names_dead = false;
      for (u32 i = 0; i < it->second.attachmentCount; ++i)
        names_dead = names_dead || it->second.attachments[i] == dead;
      if (!names_dead) {
        ++it;
        continue;
      }
      if (s.bound_framebuffer == it->second.fb.get())
        s.bound_framebuffer = nullptr;
      ParkFramebuffer(s, std::move(it->second.fb));
      it = s.framebuffers.erase(it);
    }
  }
  for (auto &fb : host.mipFramebuffers)
    ParkFramebuffer(s, std::move(fb));
  host.mipFramebuffers.clear();
  for (auto &v : host.mipViews)
    ParkView(s, std::move(v));
  host.mipViews.clear();
  ParkView(s, std::move(host.srv));
  ParkTexture(s, std::move(host.texture));
  host.layout = plume::RenderTextureLayout::UNKNOWN;
}

bool CreateOrRecycleHostTexture(VideoState &s, HostTexture &host,
                                const plume::RenderTextureDesc &desc, const char *tag) {
  const bool renderable = (desc.flags & plume::RenderTextureFlag::RENDER_TARGET) ||
                          (desc.flags & plume::RenderTextureFlag::DEPTH_TARGET);
  for (size_t i = 0; i < s.host_texture_pool.size(); ++i) {
    auto &entry = s.host_texture_pool[i];
    if (entry.freedFrame + kNumFrames > s.guest_frames ||
        !SameTextureShape(entry.host, host, desc))
      continue;
    HostTexture pooled = std::move(entry.host);
    s.host_texture_pool.erase(s.host_texture_pool.begin() + i);
    host.texture = std::move(pooled.texture);
    host.desc = pooled.desc;
    host.srv = std::move(pooled.srv);
    host.descriptorIndex = pooled.descriptorIndex;
    host.swizzledSrvs = std::move(pooled.swizzledSrvs);
    host.mipViews = std::move(pooled.mipViews);
    host.mipFramebuffers = std::move(pooled.mipFramebuffers);
    host.layout = pooled.layout;
    host.needsClear = renderable;
    s.perf.host_tex_recycled++;
    return true;
  }
  host.texture = CreateHostTexture(s.device.get(), desc, tag);
  host.desc = desc;
  host.desc.optimizedClearValue = nullptr;
  host.layout = plume::RenderTextureLayout::UNKNOWN;
  host.needsClear = renderable && host.texture != nullptr;
  return host.texture != nullptr;
}

void EvictHostTexturePool(VideoState &s) {
  size_t i = 0;
  while (i < s.host_texture_pool.size()) {
    const bool idle =
        s.host_texture_pool[i].freedFrame + kHostTexturePoolFrames < s.guest_frames;
    const bool over = s.host_texture_pool.size() > kHostTexturePoolMax;
    if (!idle && !over) {
      ++i;
      continue;
    }
    DestroyHostTexture(s, s.host_texture_pool[i].host);
    s.host_texture_pool.erase(s.host_texture_pool.begin() + i);
  }
  s.perf.pool_size = static_cast<u32>(s.host_texture_pool.size());
}

void TransitionLocked(VideoState &s, HostTexture &host, plume::RenderTextureLayout layout) {
  if (!host.texture || host.layout == layout || !s.command_list_open)
    return;
  if (s.defer_shader_read_transitions && layout == plume::RenderTextureLayout::SHADER_READ &&
      s.pending_transition_count < kMaxPendingTransitions) {
    s.pending_transitions[s.pending_transition_count++] = {&host, layout};
    return;
  }
  const HostTextureTransition transition{&host, layout};
  TransitionManyLocked(s, &transition, 1);
}

void TransitionManyLocked(VideoState &s, const HostTextureTransition *transitions, u32 count) {
  if (!transitions || !count || !s.command_list_open)
    return;
  constexpr u32 kBatch = 8;
  for (u32 first = 0; first < count; first += kBatch) {
    plume::RenderTextureBarrier barriers[kBatch];
    u32 barrier_count = 0;
    plume::RenderBarrierStages stages = plume::RenderBarrierStage::NONE;
    const u32 end = std::min(first + kBatch, count);
    for (u32 i = first; i < end; ++i) {
      HostTexture *host = transitions[i].host;
      const plume::RenderTextureLayout layout = transitions[i].layout;
      if (!host || !host->texture || host->layout == layout)
        continue;
      barriers[barrier_count++] = plume::RenderTextureBarrier(host->texture.get(), layout);
      if (layout == plume::RenderTextureLayout::COPY_SOURCE ||
          layout == plume::RenderTextureLayout::COPY_DEST)
        stages |= plume::RenderBarrierStage::COPY;
      else
        stages |= plume::RenderBarrierStage::GRAPHICS;
      host->layout = layout;
    }
    if (!barrier_count)
      continue;
    s.command_list->barriers(stages, barriers, barrier_count);
    s.perf.texture_barrier_calls++;
    s.perf.texture_barrier_resources += barrier_count;
  }
}

}
