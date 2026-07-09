/**
 * @file    gpu/device/device.cpp
 * @brief   The Plume renderer device: creation and resource-creation entry
 *          points only (see device.h for what's deliberately not here yet).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device/device.h"

#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <plume_d3d12.h>
#include <rex/runtime.h>

#include "core/logging.h"
#include "gpu/device/host_heap_arena.h"
#include "gpu/device/host_resource_heap.h"
#include "gpu/device/texture_upload.h"
#include "platform/native_window.h"

#include "src/gpu/shaders/hlsl/present_blit_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/present_blit_vs.hlsl.dxil.h"

namespace plume {
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}

namespace eot::gpu {

namespace {

std::atomic<bool> g_device_lost{false};

std::mutex g_destroy_pending_mutex;
std::unordered_set<u32> g_destroy_pending;

void DestroyResourceNow(u32 guest_va, ResourceType type) {
  auto *memory = REX_KERNEL_MEMORY();
  void *host = memory->TranslateVirtual<void *>(guest_va);
  switch (type) {
  case ResourceType::Texture:
  case ResourceType::VolumeTexture:
  case ResourceType::RenderTarget:
  case ResourceType::DepthStencil: {
    auto *tex = static_cast<GuestTexture *>(host);
    Video::NotifyTextureDestroyed(tex);
    ForgetTextureUpload(tex);
    if (tex->mappedMemory) {
      memory->SystemHeapFree(tex->mappedMemory);
      tex->mappedMemory = 0;
    }
    HostResourceHeap::Free(tex);
    break;
  }
  default:
    break;
  }
}

void DrainDeferredDestroys(VideoState &s, u32 slot) {
  std::vector<VideoState::PendingDestroy> batch;
  {
    std::lock_guard lock(s.mutex);
    batch.swap(s.deferred_destroy[slot]);
  }
  if (batch.empty())
    return;
  for (const auto &entry : batch) {
    {
      std::lock_guard lock(g_destroy_pending_mutex);
      g_destroy_pending.erase(entry.guest_va);
    }
    DestroyResourceNow(entry.guest_va, entry.type);
  }
}

}

VideoState &state() {
  static VideoState s;
  return s;
}

bool Video::CreateHostDevice() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (s.ready) {
    return true;
  }

  s.render_iface = plume::CreateD3D12Interface();
  if (!s.render_iface) {
    EOT_ERROR("Plume CreateD3D12Interface failed");
    return false;
  }
  s.device = s.render_iface->createDevice();
  if (!s.device) {
    EOT_ERROR("Plume RenderInterface::createDevice failed");
    return false;
  }

  s.queue = s.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  if (!s.queue) {
    EOT_ERROR("Plume createCommandQueue failed");
    return false;
  }
  for (u32 i = 0; i < kNumFrames; ++i) {
    s.command_lists[i] = s.queue->createCommandList();
    s.fences[i] = s.device->createCommandFence();
    s.acquire_semaphores[i] = s.device->createCommandSemaphore();
    if (!s.command_lists[i] || !s.fences[i] || !s.acquire_semaphores[i]) {
      EOT_ERROR("Plume frame-ring object creation failed for slot {}", i);
      return false;
    }
  }

  if (!HostHeapArena::Get().Init()) {
    EOT_ERROR("HostHeapArena init failed");
    return false;
  }

  s.ready = true;
  EOT_INFO("Video::CreateHostDevice: device created ({})",
           s.device->getDescription().name);
  return true;
}

bool BuildFramebuffers(VideoState &s) {
  s.framebuffers.clear();
  const u32 count = s.swap_chain->getTextureCount();
  s.framebuffers.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    plume::RenderTexture *tex = s.swap_chain->getTexture(i);
    const plume::RenderTexture *color_attachments[1] = {tex};
    plume::RenderFramebufferDesc desc(color_attachments, 1);
    auto fb = s.device->createFramebuffer(desc);
    if (!fb) {
      EOT_ERROR("Plume createFramebuffer failed for back buffer {}", i);
      s.framebuffers.clear();
      return false;
    }
    s.framebuffers.push_back(std::move(fb));
  }
  return true;
}

bool BuildPresentSemaphores(VideoState &s) {
  s.render_semaphores.clear();
  const u32 count = s.swap_chain->getTextureCount();
  s.render_semaphores.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    auto sem = s.device->createCommandSemaphore();
    if (!sem) {
      EOT_ERROR("Plume createCommandSemaphore failed for present semaphore {}",
                i);
      s.render_semaphores.clear();
      return false;
    }
    s.render_semaphores.push_back(std::move(sem));
  }
  return true;
}

namespace {

bool BuildBlitPipelineLocked(VideoState &s) {
  s.blit_vs = s.device->createShader(g_present_blit_vs_dxil,
                                     sizeof(g_present_blit_vs_dxil), "main",
                                     plume::RenderShaderFormat::DXIL);
  s.blit_ps = s.device->createShader(g_present_blit_ps_dxil,
                                     sizeof(g_present_blit_ps_dxil), "main",
                                     plume::RenderShaderFormat::DXIL);
  if (!s.blit_vs || !s.blit_ps) {
    EOT_ERROR("Present blit: shader creation failed");
    return false;
  }

  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  s.blit_sampler = s.device->createSampler(sampler_desc);
  if (!s.blit_sampler) {
    EOT_ERROR("Present blit: sampler creation failed");
    return false;
  }

  plume::RenderDescriptorRange ranges[2] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0,
                                   1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1,
                                   1)};
  plume::RenderDescriptorSetDesc set_desc(ranges, 2);
  s.blit_descriptor_set = s.device->createDescriptorSet(set_desc);
  if (!s.blit_descriptor_set) {
    EOT_ERROR("Present blit: descriptor set creation failed");
    return false;
  }
  s.blit_descriptor_set->setSampler(1, s.blit_sampler.get());

  plume::RenderPipelineLayoutDesc layout_desc;
  layout_desc.descriptorSetDescs = &set_desc;
  layout_desc.descriptorSetDescsCount = 1;
  s.blit_layout = s.device->createPipelineLayout(layout_desc);
  if (!s.blit_layout) {
    EOT_ERROR("Present blit: pipeline layout creation failed");
    return false;
  }

  plume::RenderGraphicsPipelineDesc pipeline_desc;
  pipeline_desc.pipelineLayout = s.blit_layout.get();
  pipeline_desc.vertexShader = s.blit_vs.get();
  pipeline_desc.pixelShader = s.blit_ps.get();
  pipeline_desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
  pipeline_desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  pipeline_desc.renderTargetCount = 1;
  pipeline_desc.primitiveTopology =
      plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  s.blit_pipeline = s.device->createGraphicsPipeline(pipeline_desc);
  if (!s.blit_pipeline) {
    EOT_ERROR("Present blit: graphics pipeline creation failed");
    return false;
  }
  return true;
}

}

bool Video::CreateSwapChain(rex::ui::Window *window) {
  if (!window) {
    EOT_ERROR("Video::CreateSwapChain called with null window");
    return false;
  }
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (s.present_ready)
    return true;
  if (!s.ready) {
    EOT_ERROR("Video::CreateSwapChain before CreateHostDevice");
    return false;
  }

  plume::RenderWindow render_window{};
  if (!eot::platform::GetNativeRenderWindow(window, render_window))
    return false;

  plume::RenderSwapChainDesc desc(render_window,
                                  plume::RenderFormat::B8G8R8A8_UNORM,
                                  kNumFrames + 1);
  s.swap_chain = s.queue->createSwapChain(desc);
  if (!s.swap_chain || s.swap_chain->isEmpty()) {
    EOT_ERROR("Plume createSwapChain failed");
    return false;
  }
  if (!BuildFramebuffers(s) || !BuildPresentSemaphores(s))
    return false;
  if (!BuildBlitPipelineLocked(s))
    return false;

  s.present_ready = true;
  EOT_INFO("Video::CreateSwapChain: {}x{}, {} back buffers",
           s.swap_chain->getWidth(), s.swap_chain->getHeight(),
           s.swap_chain->getTextureCount());
  return true;
}

void Video::RequestResize() {
  state().resize_requested.store(true, std::memory_order_release);
}

u32 Video::OutputWidth() {
  auto &s = state();
  return s.swap_chain ? s.swap_chain->getWidth() : 0u;
}

u32 Video::OutputHeight() {
  auto &s = state();
  return s.swap_chain ? s.swap_chain->getHeight() : 0u;
}

namespace {

void RebuildSwapChainLocked(VideoState &s) {
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
  s.framebuffers.clear();
  if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
      !BuildPresentSemaphores(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      EOT_ERROR("Swap chain resize failed");
  }
}

void LogPresentFallbackOnce(const GuestTexture *front_buffer,
                            const VideoState &s) {
  static std::atomic<bool> logged{false};
  if (logged.exchange(true, std::memory_order_relaxed))
    return;
  if (!front_buffer) {
    EOT_INFO("Present: no guest front buffer resolved; clearing instead");
  } else if (!front_buffer->texture) {
    EOT_INFO("Present: guest front buffer has no host texture; clearing");
  } else if (front_buffer->sampleCount != plume::RenderSampleCount::COUNT_1) {
    EOT_INFO("Present: guest front buffer is multi-sampled; clearing (needs a "
             "resolve before the blit can sample it)");
  } else {
    EOT_INFO("Present: guest front buffer {}x{} fmt={} not blittable onto back "
             "buffer {}x{}; clearing",
             front_buffer->width, front_buffer->height,
             static_cast<u32>(front_buffer->format), s.swap_chain->getWidth(),
             s.swap_chain->getHeight());
  }
}

void AdvanceAndWaitReusedLocked(VideoState &s) {
  const u32 slot = s.next_frame;
  s.frame.store(slot, std::memory_order_relaxed);
  s.next_frame = (slot + 1) % kNumFrames;
  if (s.command_list_submitted[slot]) {
    s.queue->waitForCommandFence(s.fences[slot].get());
    s.command_list_submitted[slot] = false;
  }
  s.blit_view_graveyard[slot].clear();
  s.upload_staging[slot].clear();
}

}

void Video::Present(GuestTexture *front_buffer) {
  auto &s = state();
  std::unique_lock lock(s.mutex);
  if (!s.present_ready || DeviceIsLost())
    return;
  if (s.frame_present_committed)
    return;

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested)
    RebuildSwapChainLocked(s);

  if (s.framebuffers.empty())
    return;

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  u32 texture_index = 0;
  if (!s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(),
                                    &texture_index)) {
    return;
  }
  if (texture_index >= s.framebuffers.size() ||
      texture_index >= s.render_semaphores.size()) {
    EOT_ERROR("Present: acquired image {} outside the built set ({} fbs)",
              texture_index, s.framebuffers.size());
    return;
  }

  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  auto *cmd = s.command_lists[cur].get();

  if (!front_buffer || !front_buffer->hasContent) {
    const u32 want_w = front_buffer ? front_buffer->width : 0;
    const u32 want_h = front_buffer ? front_buffer->height : 0;
    if (GuestTexture *uploaded = LastUploadedTexture(want_w, want_h)) {
      if (uploaded->texture && uploaded->hasContent)
        front_buffer = uploaded;
    }
  }

  const bool blittable =
      front_buffer && front_buffer->texture && front_buffer->width &&
      front_buffer->height &&
      front_buffer->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      s.blit_pipeline != nullptr;

  plume::RenderTextureView *src_view = nullptr;
  if (blittable) {
    plume::RenderTextureViewDesc view_desc;
    view_desc.format = front_buffer->format;
    view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    view_desc.mipLevels = 1;
    auto view = front_buffer->texture->createTextureView(view_desc);
    if (view) {
      src_view = view.get();
      s.blit_view_graveyard[cur].push_back(std::move(view));
    }
  }

  static GuestTexture *reported_source = reinterpret_cast<GuestTexture *>(1);
  static u32 reported_count = 0;
  if (reported_source != front_buffer && reported_count < 4) {
    reported_source = front_buffer;
    ++reported_count;
    EOT_INFO("[present] front={} tex={} {}x{} fmt={} samples={} pipeline={} -> "
             "{}",
             front_buffer != nullptr,
             front_buffer && front_buffer->texture,
             front_buffer ? front_buffer->width : 0,
             front_buffer ? front_buffer->height : 0,
             front_buffer ? static_cast<u32>(front_buffer->format) : 0u,
             front_buffer ? static_cast<u32>(front_buffer->sampleCount) : 0u,
             s.blit_pipeline != nullptr,
             blittable ? "BLIT" : "clear only");
  }

  cmd->begin();

  FlushTextureUploads(cmd, s.upload_staging[cur]);

  if (src_view) {
    plume::RenderTextureBarrier to_blit[] = {
        plume::RenderTextureBarrier(front_buffer->texture,
                                    plume::RenderTextureLayout::SHADER_READ),
        plume::RenderTextureBarrier(back,
                                    plume::RenderTextureLayout::COLOR_WRITE)};
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, to_blit, 2);
    front_buffer->layout = plume::RenderTextureLayout::SHADER_READ;

    s.blit_descriptor_set->setTexture(0, front_buffer->texture,
                                      plume::RenderTextureLayout::SHADER_READ,
                                      src_view);
    const u32 w = s.swap_chain->getWidth();
    const u32 h = s.swap_chain->getHeight();
    cmd->setFramebuffer(back_fb);
    cmd->setPipeline(s.blit_pipeline.get());
    cmd->setGraphicsPipelineLayout(s.blit_layout.get());
    cmd->setGraphicsDescriptorSet(s.blit_descriptor_set.get(), 0);
    cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(w), float(h)));
    cmd->setScissors(
        plume::RenderRect(0, 0, static_cast<i32>(w), static_cast<i32>(h)));
    cmd->drawInstanced(3, 1, 0, 0);
    cmd->setFramebuffer(nullptr);
  } else {
    LogPresentFallbackOnce(front_buffer, s);
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                  plume::RenderTextureBarrier(
                      back, plume::RenderTextureLayout::COLOR_WRITE));
    cmd->setFramebuffer(back_fb);
    cmd->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    cmd->setFramebuffer(nullptr);
  }
  cmd->barriers(plume::RenderBarrierStage::GRAPHICS,
                plume::RenderTextureBarrier(
                    back, plume::RenderTextureLayout::PRESENT));
  cmd->end();

  const plume::RenderCommandList *lists[] = {cmd};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;

  if (!s.swap_chain->present(texture_index, signals, 1))
    CheckDeviceRemoved("swapchain present");

  s.frame_present_committed = true;
  AdvanceAndWaitReusedLocked(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainDeferredDestroys(s, reclaimed);
}

plume::RenderDevice *Video::HostDevice() { return state().device.get(); }

u32 Video::BindTextureSRV(GuestTexture *) {
  return kInvalidDescriptorIndex;
}

void Video::QueueResourceDestroy(u32 guest_va, ResourceType type) {
  if (!guest_va)
    return;
  {
    std::lock_guard lock(g_destroy_pending_mutex);
    if (!g_destroy_pending.insert(guest_va).second)
      return;
  }
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.present_ready) {
    {
      std::lock_guard pending(g_destroy_pending_mutex);
      g_destroy_pending.erase(guest_va);
    }
    DestroyResourceNow(guest_va, type);
    return;
  }
  const u32 slot = s.frame.load(std::memory_order_relaxed);
  s.deferred_destroy[slot].push_back({guest_va, type});
}

namespace {

plume::RenderTextureView *AttachmentViewLocked(GuestTexture *tex) {
  if (!tex || !tex->texture)
    return nullptr;
  if (!tex->attachmentView) {
    plume::RenderTextureViewDesc desc;
    desc.format = tex->format;
    desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    desc.mipLevels = 1;
    tex->attachmentView = tex->texture->createTextureView(desc);
    if (!tex->attachmentView) {
      EOT_ERROR("Attachment view creation failed ({}x{} fmt={})", tex->width,
                tex->height, static_cast<u32>(tex->format));
      return nullptr;
    }
  }
  return tex->attachmentView.get();
}

plume::RenderFramebuffer *GetFramebufferLocked(VideoState &s, GuestTexture *rt,
                                              GuestTexture *ds) {
  if (rt && !rt->texture)
    rt = nullptr;
  if (ds && !ds->texture)
    ds = nullptr;
  if (!rt && !ds)
    return nullptr;

  GuestTexture *owner = rt ? rt : ds;
  const plume::RenderTexture *key =
      rt ? (ds ? ds->texture : nullptr) : nullptr;
  auto it = owner->framebuffers.find(key);
  if (it != owner->framebuffers.end())
    return it->second.get();

  if (rt && !AttachmentViewLocked(rt))
    return nullptr;
  if (ds && !AttachmentViewLocked(ds))
    return nullptr;

  const plume::RenderTexture *colors[1] = {rt ? rt->texture : nullptr};
  plume::RenderFramebufferDesc desc;
  if (rt) {
    desc.colorAttachments = colors;
    desc.colorAttachmentsCount = 1;
  }
  if (ds)
    desc.depthAttachment = ds->texture;

  auto fb = s.device->createFramebuffer(desc);
  if (!fb) {
    EOT_ERROR("createFramebuffer failed: colour={} depth={} ({}x{} fmt={})",
              rt ? "yes" : "no", ds ? "yes" : "no", owner->width, owner->height,
              static_cast<u32>(owner->format));
    return nullptr;
  }
  auto *raw = fb.get();
  owner->framebuffers.emplace(key, std::move(fb));
  s.framebuffer_owners.insert(owner);
  return raw;
}

}

void Video::SetRenderTarget(u32 index, GuestTexture *surface) {
  if (index >= kMaxRenderTargets)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (s.render_targets[index] != surface) {
    s.render_targets[index] = surface;
    if (index == 0)
      s.draw_framebuffer_bound = false;
  }
}

void Video::SetDepthStencil(GuestTexture *surface) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (s.depth_stencil != surface) {
    s.depth_stencil = surface;
    s.draw_framebuffer_bound = false;
  }
}

void Video::SetVertexShader(GuestShader *shader) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.vertex_shader = shader;
}

void Video::SetPixelShader(GuestShader *shader) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.pixel_shader = shader;
}

GuestShader *Video::BoundVertexShader() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return s.vertex_shader;
}

GuestShader *Video::BoundPixelShader() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return s.pixel_shader;
}

void Video::SetStreamSource(u32 stream, GuestBuffer *buffer, u32 offset,
                            u32 stride) {
  if (stream >= kMaxStreamSources)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.streams[stream] = {buffer, offset, stride};
}

u32 Video::BoundStreamStride(u32 stream) {
  if (stream >= kMaxStreamSources)
    return 0;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return s.streams[stream].stride;
}

Video::AttachmentFormats Video::BoundAttachmentFormats() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  AttachmentFormats out;
  if (s.render_targets[0]) {
    out.color = s.render_targets[0]->format;
    out.sampleCount =
        static_cast<u32>(s.render_targets[0]->sampleCount);
  }
  if (s.depth_stencil)
    out.depth = s.depth_stencil->format;
  return out;
}

void Video::SetIndices(GuestBuffer *buffer) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.index_buffer = buffer;
}

Video::FramebufferBind Video::BindDrawFramebuffer() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.present_ready)
    return FramebufferBind::kNotReady;

  GuestTexture *rt = s.render_targets[0];
  GuestTexture *ds = s.depth_stencil;
  if (!rt && !ds)
    return FramebufferBind::kNothingBound;

  if (s.draw_framebuffer_bound && rt == s.bound_fb_rt && ds == s.bound_fb_ds)
    return FramebufferBind::kBound;

  if (rt && !rt->texture)
    return FramebufferBind::kNoHostTexture;
  if (!rt && !ds->texture)
    return FramebufferBind::kNoHostTexture;

  plume::RenderFramebuffer *fb = GetFramebufferLocked(s, rt, ds);
  if (!fb)
    return FramebufferBind::kCreateFailed;

  s.bound_fb_rt = rt;
  s.bound_fb_ds = ds;
  s.draw_framebuffer_bound = true;
  return FramebufferBind::kBound;
}

void Video::NotifyTextureDestroyed(GuestTexture *dead) {
  if (!dead)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);

  for (auto *&slot : s.render_targets) {
    if (slot == dead)
      slot = nullptr;
  }
  if (s.depth_stencil == dead)
    s.depth_stencil = nullptr;
  if (s.bound_fb_rt == dead || s.bound_fb_ds == dead) {
    s.bound_fb_rt = nullptr;
    s.bound_fb_ds = nullptr;
    s.draw_framebuffer_bound = false;
  }

  dead->framebuffers.clear();
  s.framebuffer_owners.erase(dead);
  for (auto *owner : s.framebuffer_owners) {
    if (dead->texture)
      owner->framebuffers.erase(dead->texture);
  }
}

void Video::BeginGuestFrame() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.frame_present_committed = false;
}

plume::RenderSampleCounts Video::CvarMSAASampleCount() {
  return plume::RenderSampleCount::COUNT_1;
}

std::unique_ptr<plume::RenderTexture>
CreateHostTexture(plume::RenderDevice *device,
                  const plume::RenderTextureDesc &desc, const char *tag) {
  if (!device) {
    EOT_ERROR("CreateHostTexture({}): no host device", tag ? tag : "?");
    return nullptr;
  }
  auto texture = device->createTexture(desc);
  if (!texture ||
      static_cast<plume::D3D12Texture *>(texture.get())->d3d == nullptr) {
    EOT_ERROR(
        "CreateHostTexture({}) failed: backend resource null ({}x{} fmt={})",
        tag ? tag : "?", desc.width, desc.height,
        static_cast<u32>(desc.format));
    CheckDeviceRemoved(tag ? tag : "texture");
    return nullptr;
  }
  return texture;
}

bool CheckDeviceRemoved(const char *context) {
  if (g_device_lost.load(std::memory_order_acquire))
    return true;

  auto *dev = static_cast<plume::D3D12Device *>(Video::HostDevice());
  if (!dev || !dev->d3d)
    return false;
  const long hr = dev->d3d->GetDeviceRemovedReason();
  if (hr == 0)
    return false;

  if (!g_device_lost.exchange(true, std::memory_order_acq_rel)) {
    EOT_ERROR("Device removed ({}): hr=0x{:08X}", context ? context : "?",
              static_cast<u32>(hr));
  }
  return true;
}

bool DeviceIsLost() { return g_device_lost.load(std::memory_order_acquire); }

}
