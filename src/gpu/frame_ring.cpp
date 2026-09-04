#include <mutex>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"

namespace eot::gpu {

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
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
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
  s.command_lists[cur]->end();
  s.command_list_open = false;
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
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
  }
  UploadRingResetFrame(slot);
}

void DrainSlot(VideoState &s, u32 slot) {
  std::lock_guard lock(s.mutex);
  s.view_graveyard[slot].clear();
  s.framebuffer_graveyard[slot].clear();
  s.texture_graveyard[slot].clear();
  s.buffer_graveyard[slot].clear();
  DrainDescriptorSlotsLocked(s, slot);
}

void ParkTexture(VideoState &s, std::unique_ptr<plume::RenderTexture> t) {
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
  ReleaseTextureSRVLocked(s, host);
  if (host.texture) {
    const auto *dead = host.texture.get();
    for (auto it = s.framebuffers.begin(); it != s.framebuffers.end();) {
      (void)dead;
      ParkFramebuffer(s, std::move(it->second));
      it = s.framebuffers.erase(it);
    }
    if (s.bound_framebuffer)
      s.bound_framebuffer = nullptr;
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

void TransitionLocked(VideoState &s, HostTexture &host, plume::RenderTextureLayout layout) {
  if (!host.texture || host.layout == layout || !s.command_list_open)
    return;
  plume::RenderTextureBarrier b(host.texture.get(), layout);
  plume::RenderBarrierStages stages = plume::RenderBarrierStage::GRAPHICS;
  if (layout == plume::RenderTextureLayout::COPY_SOURCE ||
      layout == plume::RenderTextureLayout::COPY_DEST)
    stages = plume::RenderBarrierStage::COPY;
  s.command_list->barriers(stages, &b, 1);
  host.layout = layout;
}

}
