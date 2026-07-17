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

#include <algorithm>
#include <atomic>
#include <set>
#include <cmath>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <plume_d3d12.h>
#include <plume_render_interface_builders.h>
#include <rex/runtime.h>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/settings.h"
#include "core/memory_helpers.h"
#include "gpu/device/host_heap_arena.h"
#include "gpu/device/host_resource_heap.h"
#include "gpu/device/rdc_capture.h"
#include "gpu/device/native_texture_mirror.h"
#include "gpu/device/texture_upload.h"
#include "gpu/guest/texture_fetch.h"
#include "gpu/pipeline/constant_buffers.h"
#include "gpu/pipeline/geometry_upload.h"
#include "platform/native_window.h"

#include "src/gpu/shaders/hlsl/present_blit_ps.hlsl.dxil.h"
#include "src/gpu/shaders/hlsl/present_blit_vs.hlsl.dxil.h"

namespace plume {
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}

REXCVAR_DEFINE_BOOL(eot_probe_resolve_green, false, kCvarGroup, "Paint resolves green (test)");

REXCVAR_DEFINE_BOOL(eot_protect_resolved, false, kCvarGroup, "Skip guest clears (test)");

namespace eot::gpu {

namespace {

std::atomic<bool> g_device_lost{false};
std::atomic<const GuestTexture *> g_last_front{nullptr};
std::atomic<u32> g_identity_probe{0};

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

bool BuildGuestPipelineLayoutLocked(VideoState &s);

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

bool BuildGuestPipelineLayoutLocked(VideoState &s) {
  plume::RenderDescriptorSetBuilder tex_builder;
  tex_builder.begin();
  tex_builder.addTexture(0, kBindlessTextureCount);
  tex_builder.end(true, kBindlessTextureCount);
  s.guest_texture_set = tex_builder.create(s.device.get());
  if (!s.guest_texture_set) {
    EOT_ERROR("Guest pipeline layout: bindless texture set creation failed");
    return false;
  }

  plume::RenderDescriptorSetBuilder sampler_builder;
  sampler_builder.begin();
  sampler_builder.addSampler(0, kBindlessSamplerCount);
  sampler_builder.end(true, kBindlessSamplerCount);
  s.guest_sampler_set = sampler_builder.create(s.device.get());
  if (!s.guest_sampler_set) {
    EOT_ERROR("Guest pipeline layout: bindless sampler set creation failed");
    return false;
  }

  plume::RenderSamplerDesc guest_sampler_desc;
  guest_sampler_desc.minFilter = plume::RenderFilter::LINEAR;
  guest_sampler_desc.magFilter = plume::RenderFilter::LINEAR;
  guest_sampler_desc.mipmapMode = plume::RenderMipmapMode::NEAREST;
  guest_sampler_desc.addressU = plume::RenderTextureAddressMode::WRAP;
  guest_sampler_desc.addressV = plume::RenderTextureAddressMode::WRAP;
  guest_sampler_desc.addressW = plume::RenderTextureAddressMode::WRAP;
  s.guest_default_sampler = s.device->createSampler(guest_sampler_desc);
  if (!s.guest_default_sampler) {
    EOT_ERROR("Guest pipeline layout: default sampler creation failed");
    return false;
  }
  s.guest_sampler_set->setSampler(0, s.guest_default_sampler.get());

  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, true);
  layout_builder.addDescriptorSet(tex_builder);
  layout_builder.addDescriptorSet(tex_builder);
  layout_builder.addDescriptorSet(tex_builder);
  layout_builder.addDescriptorSet(sampler_builder);
  layout_builder.addDescriptorSet(tex_builder);
  layout_builder.addRootDescriptor(
      0, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  layout_builder.addRootDescriptor(
      1, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  layout_builder.addRootDescriptor(
      2, 4, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
  layout_builder.end();

  s.guest_pipeline_layout = layout_builder.create(s.device.get());
  if (!s.guest_pipeline_layout) {
    EOT_ERROR("Guest pipeline layout: createPipelineLayout failed");
    return false;
  }
  plume::RenderTextureDesc null_desc = plume::RenderTextureDesc::Texture2D(
      1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM);
  s.null_texture = s.device->createTexture(null_desc);
  if (s.null_texture) {
    plume::RenderTextureViewDesc null_view_desc;
    null_view_desc.format = plume::RenderFormat::R8G8B8A8_UNORM;
    null_view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    null_view_desc.mipLevels = 1;
    s.null_texture_view = s.null_texture->createTextureView(null_view_desc);
  }
  if (!s.null_texture || !s.null_texture_view) {
    EOT_ERROR("Guest pipeline layout: null texture descriptor creation failed");
    return false;
  }
  s.guest_texture_set->setTexture(0, s.null_texture.get(),
                                  plume::RenderTextureLayout::SHADER_READ,
                                  s.null_texture_view.get());

  EOT_INFO("Guest pipeline layout built: {} textures, {} samplers, 3 CBVs",
           kBindlessTextureCount, kBindlessSamplerCount);
  return true;
}

plume::RenderPipelineLayout *Video::GuestPipelineLayout() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.guest_pipeline_layout && !s.guest_layout_failed) {
    if (!BuildGuestPipelineLayoutLocked(s))
      s.guest_layout_failed = true;
  }
  return s.guest_pipeline_layout.get();
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
  for (u32 i = 0; i < kNumFrames; ++i)
    s.blit_descriptor_set[i] = s.device->createDescriptorSet(set_desc);
  if (!s.blit_descriptor_set[0]) {
    EOT_ERROR("Present blit: descriptor set creation failed");
    return false;
  }
  for (u32 i = 0; i < kNumFrames; ++i)
    s.blit_descriptor_set[i]->setSampler(1, s.blit_sampler.get());

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

void EnsureCommandListOpenLocked(VideoState &s, u32 slot) {
  if (slot >= kNumFrames || s.command_list_open[slot])
    return;
  if (auto *cmd = s.command_lists[slot].get()) {
    cmd->begin();
    s.command_list_open[slot] = true;
  }
}

void AdvanceAndWaitReusedLocked(VideoState &s) {
  ++s.frame_serial;
  {
    static std::atomic<u32> advances{0};
    if ((advances.fetch_add(1, std::memory_order_relaxed) % 500) == 499)
      EOT_INFO("[present] {} ring advances so far (serial {})",
               advances.load(), s.frame_serial);
  }
  DrainValidationMessages();
  CheckDeviceRemoved("present");
  const u32 slot = s.next_frame;
  s.frame.store(slot, std::memory_order_relaxed);
  s.next_frame = (slot + 1) % kNumFrames;
  if (s.command_list_submitted[slot]) {
    s.queue->waitForCommandFence(s.fences[slot].get());
    s.command_list_submitted[slot] = false;
  }
  s.blit_view_graveyard[slot].clear();
  s.upload_staging[slot].Reset();
  s.resolve_set_used[slot] = 0;
  constants::ResetFrame(slot);
  ResetGeometryFrame();
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

  if (front_buffer && !front_buffer->hasContent && front_buffer->selfVa) {
    GuestTextureFetch fetch;
    if (DecodeTextureFetchAt(front_buffer->selfVa + kTextureObjectFetchOffset,
                             fetch) &&
        fetch.baseAddress) {
      if (GuestTexture *twin = ResolveMirrorByAddress(fetch.baseAddress);
          twin && twin != front_buffer && twin->texture && twin->hasContent) {
        static bool reported = false;
        if (!reported) {
          reported = true;
          EOT_INFO("[present] front buffer 0x{:08X} has no content; presenting "
                   "the resolved surface sharing its page instead",
                   front_buffer->selfVa);
        }
        front_buffer = twin;
      }
    }
  }

  if (GuestTexture *busiest = s.busiest_rt;
      busiest && busiest->texture && busiest->hasContent &&
      s.busiest_rt_serial == s.frame_serial) {
    const u32 front_draws =
        (s.last_front_src && s.front_resolve_serial == s.frame_serial)
            ? s.last_front_src->drawsThisFrame
            : 0;
    if (s.busiest_rt_draws > front_draws * 4) {
      static std::atomic<u32> reported{0};
      if (reported.fetch_add(1, std::memory_order_relaxed) == 0) {
        EOT_INFO("[present] the composite the guest resolved carries {} draws "
                 "against {} in the frame's scene surface; presenting the "
                 "scene",
                 front_draws, s.busiest_rt_draws);
      }
      front_buffer = busiest;
    }
  }

  {
    const GuestTexture *busiest = s.busiest_rt;
    static std::atomic<u32> ticks{0};
    if (s.frame_draw_total >= 20 &&
        (ticks.fetch_add(1, std::memory_order_relaxed) % 60) == 0) {
      EOT_INFO("[present] {} colour draws over {} surfaces (peak {}); {} front "
               "resolves, last from {} ({} draws); front={} "
               "(draws={} content={}) busiest={} ({}x{}, {} draws)",
               s.frame_draw_total, s.frame_surface_count, s.peak_frame_draws,
               s.front_resolves, static_cast<const void *>(s.last_front_src),
               (s.last_front_src && s.front_resolve_serial == s.frame_serial)
                   ? s.last_front_src->drawsThisFrame
                   : 0,
               static_cast<const void *>(front_buffer),
               front_buffer ? front_buffer->drawsThisFrame : 0,
               front_buffer ? front_buffer->hasContent : false,
               static_cast<const void *>(busiest),
               busiest ? busiest->width : 0, busiest ? busiest->height : 0,
               s.busiest_rt_draws);
    }
  }

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
    if (front_buffer && front_buffer->selfVa) {
      GuestTextureFetch ff;
      DecodeTextureFetchAt(front_buffer->selfVa + kTextureObjectFetchOffset,
                           ff);
      EOT_INFO("[present] front va=0x{:08X} base=0x{:08X} page=0x{:X}",
               front_buffer->selfVa, ff.baseAddress, ff.baseAddress >> 12);
    }
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

  EnsureCommandListOpenLocked(s, cur);

  if (!s.null_texture_filled && s.null_texture) {
    s.null_texture_filled = true;
    auto staging = s.device->createBuffer(
        plume::RenderBufferDesc::UploadBuffer(256));
    if (staging) {
      if (void *mapped = staging->map()) {
        const u8 magenta[4] = {255, 0, 255, 255};
        std::memcpy(mapped, magenta, sizeof(magenta));
        staging->unmap();
        const plume::RenderTextureBarrier to_copy(
            s.null_texture.get(), plume::RenderTextureLayout::COPY_DEST);
        cmd->barriers(plume::RenderBarrierStage::COPY, &to_copy, 1);
        cmd->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(s.null_texture.get(),
                                                          0, 0),
            plume::RenderTextureCopyLocation::PlacedFootprint(
                staging.get(), plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1,
                64));
        const plume::RenderTextureBarrier to_read(
            s.null_texture.get(), plume::RenderTextureLayout::SHADER_READ);
        cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1);
      }
      s.null_fill_staging = std::move(staging);
    }
  }

  cmd->setFramebuffer(nullptr);

  FlushTextureUploads(cmd, s.upload_staging[cur]);

  if (src_view) {
    plume::RenderTextureBarrier to_blit[] = {
        plume::RenderTextureBarrier(front_buffer->texture,
                                    plume::RenderTextureLayout::SHADER_READ),
        plume::RenderTextureBarrier(back,
                                    plume::RenderTextureLayout::COLOR_WRITE)};
    cmd->barriers(plume::RenderBarrierStage::GRAPHICS, to_blit, 2);
    front_buffer->layout = plume::RenderTextureLayout::SHADER_READ;

    auto *blit_set = s.blit_descriptor_set[cur].get();
    blit_set->setTexture(0, front_buffer->texture,
                         plume::RenderTextureLayout::SHADER_READ, src_view);
    const u32 w = s.swap_chain->getWidth();
    const u32 h = s.swap_chain->getHeight();
    cmd->setFramebuffer(back_fb);
    cmd->setPipeline(s.blit_pipeline.get());
    cmd->setGraphicsPipelineLayout(s.blit_layout.get());
    cmd->setGraphicsDescriptorSet(blit_set, 0);
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
  s.command_list_open[cur] = false;

  const plume::RenderCommandList *lists[] = {cmd};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;

  if (!s.swap_chain->present(texture_index, signals, 1))
    CheckDeviceRemoved("swapchain present");

  {
    static std::atomic<bool> armed{true};
    if (const char *want = getenv("EOT_RDC_BUSY_FRAME")) {
      const u32 threshold = static_cast<u32>(std::atoi(want));
      if (threshold && s.frame_draw_total >= threshold &&
          armed.exchange(false)) {
        EOT_INFO("[rdc] frame carries {} colour draws; asking for a capture",
                 s.frame_draw_total);
        TriggerCaptureNow();
      }
    }
  }

  NotePresentForCapture();
  g_last_front.store(front_buffer, std::memory_order_relaxed);
  s.frame_present_committed = true;
  AdvanceAndWaitReusedLocked(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainDeferredDestroys(s, reclaimed);
  DrainEvictedNativeTextures(reclaimed);
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
  switch (type) {
  case ResourceType::Texture:
  case ResourceType::VolumeTexture:
  case ResourceType::RenderTarget:
  case ResourceType::DepthStencil: {
    auto *memory = REX_KERNEL_MEMORY();
    ForgetTextureUpload(memory->TranslateVirtual<GuestTexture *>(guest_va));
    break;
  }
  default:
    break;
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

  static std::atomic<u32> not_target{0};
  if (rt && !(rt->desc_flags & plume::RenderTextureFlag::RENDER_TARGET)) {
    if (not_target.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_WARN("[fb] colour attachment {}x{} fmt={} was not created as a "
               "render target; skipping the framebuffer",
               rt->width, rt->height, static_cast<u32>(rt->format));
    }
    return nullptr;
  }

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

void Video::NoteAttachmentsDrawnLocked(GuestTexture *color,
                                       GuestTexture *depth) {
  auto &s = state();
  const u32 slot = s.frame.load(std::memory_order_relaxed);
  if (color) {
    if (color->drawnSerial != s.frame_serial) {
      color->drawnSerial = s.frame_serial;
      color->drawsThisFrame = 0;
    }
    ++color->drawsThisFrame;
    color->hasContent = true;
    s.last_drawn_rt[slot] = color;
    if (s.busiest_rt_serial != s.frame_serial) {
      s.busiest_rt_serial = s.frame_serial;
      s.busiest_rt = nullptr;
      s.busiest_rt_draws = 0;
      s.frame_draw_total = 0;
      s.frame_surface_count = 0;
    }
    ++s.frame_draw_total;
    {
      static std::atomic<u32> peak{0};
      u32 seen = peak.load(std::memory_order_relaxed);
      while (s.frame_draw_total > seen &&
             !peak.compare_exchange_weak(seen, s.frame_draw_total))
        ;
      s.peak_frame_draws = peak.load(std::memory_order_relaxed);
    }
    {
      bool known = false;
      for (u32 i = 0; i < s.frame_surface_count; ++i)
        known = known || s.frame_surfaces[i] == color;
      if (!known && s.frame_surface_count < 16)
        s.frame_surfaces[s.frame_surface_count++] = color;
    }
    if (color->drawsThisFrame >= s.busiest_rt_draws) {
      s.busiest_rt = color;
      s.busiest_rt_draws = color->drawsThisFrame;
    }
  }
  if (depth) {
    depth->drawnSerial = s.frame_serial;
    s.last_drawn_ds[slot] = depth;
  }
}

u32 Video::CurrentFrameSlot() {
  return state().frame.load(std::memory_order_relaxed);
}

void Video::ReleaseTextureDescriptor(u32 index) {
  if (index == kInvalidDescriptorIndex || index == 0)
    return;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (s.guest_texture_set && s.null_texture && s.null_texture_view) {
    s.guest_texture_set->setTexture(index, s.null_texture.get(),
                                    plume::RenderTextureLayout::SHADER_READ,
                                    s.null_texture_view.get());
  }
  s.free_texture_slots.push_back(index);
}

plume::RenderDescriptorSet *Video::GuestTextureSet() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return s.guest_texture_set.get();
}

plume::RenderDescriptorSet *Video::GuestSamplerSet() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return s.guest_sampler_set.get();
}

u32 Video::AcquireTextureDescriptor(GuestTexture *tex) {
  if (!tex || !tex->texture)
    return kInvalidDescriptorIndex;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (tex->descriptorIndex != kInvalidDescriptorIndex)
    return tex->descriptorIndex;
  if (!s.guest_texture_set)
    return kInvalidDescriptorIndex;
  if (!s.free_texture_slots.empty()) {
    const u32 slot = s.free_texture_slots.back();
    s.free_texture_slots.pop_back();
    plume::RenderTextureViewDesc view_desc;
    view_desc.format = tex->format;
    view_desc.dimension = tex->viewDimension;
    view_desc.mipLevels = tex->mipLevels ? tex->mipLevels : 1;
    auto view = tex->texture->createTextureView(view_desc);
    if (!view)
      return kInvalidDescriptorIndex;
    s.guest_texture_set->setTexture(slot, tex->texture,
                                    plume::RenderTextureLayout::SHADER_READ,
                                    view.get());
    tex->textureView = std::move(view);
    tex->descriptorIndex = slot;
    return slot;
  }
  if (s.next_texture_slot >= kBindlessTextureCount) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      EOT_ERROR("[bindless] texture heap exhausted at {} slots; slots are not "
                "recycled yet", kBindlessTextureCount);
    }
    return kInvalidDescriptorIndex;
  }

  plume::RenderTextureViewDesc view_desc;
  view_desc.format = tex->format;
  view_desc.dimension = tex->viewDimension;
  view_desc.mipLevels = tex->mipLevels ? tex->mipLevels : 1;
  auto view = tex->texture->createTextureView(view_desc);
  if (!view)
    return kInvalidDescriptorIndex;

  const u32 slot = s.next_texture_slot++;
  s.guest_texture_set->setTexture(slot, tex->texture,
                                  plume::RenderTextureLayout::SHADER_READ,
                                  view.get());
  tex->textureView = std::move(view);
  tex->descriptorIndex = slot;
  return slot;
}

Video::BoundStreamInfo Video::BoundStream(u32 stream) {
  BoundStreamInfo out;
  if (stream >= kMaxStreamSources)
    return out;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  const VideoState::StreamSource &src = s.streams[stream];
  if (!src.buffer)
    return out;
  out.address = src.address();
  out.stride = src.stride;
  out.size = src.offset < src.buffer->size ? src.buffer->size - src.offset : 0;
  return out;
}

Video::BoundIndexInfo Video::BoundIndexBuffer() {
  BoundIndexInfo out;
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.index_buffer)
    return out;
  out.address = s.index_buffer->address;
  out.size = s.index_buffer->size;
  out.index32 = s.index_buffer->index32;
  return out;
}

GuestTexture *Video::BoundDepthTexture() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return s.depth_stencil;
}

GuestTexture *Video::BoundColorTexture() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  return s.render_targets[0];
}

Video::AttachmentSize Video::BoundAttachmentSize() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  AttachmentSize out;
  const GuestTexture *tex = s.render_targets[0] ? s.render_targets[0]
                                                : s.depth_stencil;
  if (tex) {
    out.width = tex->width;
    out.height = tex->height;
  }
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
  s.bound_framebuffer = nullptr;

  if (rt && !rt->texture)
    return FramebufferBind::kNoHostTexture;
  if (!rt && !ds->texture)
    return FramebufferBind::kNoHostTexture;

  plume::RenderFramebuffer *fb = GetFramebufferLocked(s, rt, ds);
  if (!fb)
    return FramebufferBind::kCreateFailed;

  s.bound_fb_rt = rt;
  s.bound_fb_ds = ds;
  s.bound_framebuffer = fb;
  s.draw_framebuffer_bound = true;
  return FramebufferBind::kBound;
}

void Video::NotifyTextureDestroyed(GuestTexture *dead) {
  if (!dead)
    return;
  ForgetTextureUpload(dead);

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
    s.bound_framebuffer = nullptr;
    s.draw_framebuffer_bound = false;
  }

  dead->framebuffers.clear();
  s.framebuffer_owners.erase(dead);
  for (auto *owner : s.framebuffer_owners) {
    if (dead->texture)
      owner->framebuffers.erase(dead->texture);
  }
}

namespace {

bool SameFormatFamily(plume::RenderFormat a, plume::RenderFormat b) {
  auto family = [](plume::RenderFormat f) {
    switch (f) {
    case plume::RenderFormat::R16G16B16A16_FLOAT:
    case plume::RenderFormat::R16G16B16A16_UNORM:
    case plume::RenderFormat::R16G16B16A16_SNORM:
    case plume::RenderFormat::R16G16B16A16_UINT:
    case plume::RenderFormat::R16G16B16A16_SINT:
    case plume::RenderFormat::R16G16B16A16_TYPELESS:
      return 1;
    case plume::RenderFormat::R8G8B8A8_UNORM:
    case plume::RenderFormat::R8G8B8A8_UINT:
    case plume::RenderFormat::R8G8B8A8_SNORM:
    case plume::RenderFormat::R8G8B8A8_SINT:
    case plume::RenderFormat::R8G8B8A8_TYPELESS:
      return 2;
    default:
      return 0;
    }
  };
  const int fa = family(a);
  return fa != 0 && fa == family(b);
}

}

namespace {

bool ResolveByBlitLocked(VideoState &s, Video::RecordingList &rec,
                         GuestTexture *src, GuestTexture *dest) {
  static std::atomic<u32> reported{0};
  const auto refuse = [&](const char *why) {
    if (reported.fetch_add(1, std::memory_order_relaxed) < 6)
      EOT_INFO("[resolve] blit refused ({}): {}x{} fmt={} -> {}x{} fmt={}", why,
               src->width, src->height, static_cast<u32>(src->format),
               dest->width, dest->height, static_cast<u32>(dest->format));
    return false;
  };
  if (!s.blit_vs || !s.blit_ps || !s.blit_layout || !s.device)
    return refuse("no blit pipeline state");
  if (IsBlockCompressed(dest->format))
    return refuse("block-compressed destination");

  const u32 fmt_key = static_cast<u32>(dest->format);
  auto pit = s.resolve_pipelines.find(fmt_key);
  if (pit == s.resolve_pipelines.end()) {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.blit_layout.get();
    desc.vertexShader = s.blit_vs.get();
    desc.pixelShader = s.blit_ps.get();
    desc.renderTargetFormat[0] = dest->format;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    auto pipeline = s.device->createGraphicsPipeline(desc);
    if (!pipeline)
      return refuse("no pipeline for this destination format");
    pit = s.resolve_pipelines.emplace(fmt_key, std::move(pipeline)).first;
  }

  const u32 cur = s.frame.load(std::memory_order_relaxed);

  if (!src->textureView) {
    plume::RenderTextureViewDesc view_desc;
    view_desc.format = src->format;
    view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    view_desc.mipLevels = 1;
    src->textureView = src->texture->createTextureView(view_desc);
  }
  if (!src->textureView)
    return refuse("source has no view");

  const u32 slot = s.resolve_set_used[cur];
  if (slot >= s.resolve_sets[cur].size()) {
    plume::RenderDescriptorRange ranges[2] = {
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE,
                                     0, 1),
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER,
                                     1, 1)};
    plume::RenderDescriptorSetDesc set_desc(ranges, 2);
    auto set = s.device->createDescriptorSet(set_desc);
    if (!set)
      return refuse("descriptor set creation failed");
    set->setSampler(1, s.blit_sampler.get());
    s.resolve_sets[cur].push_back(std::move(set));
  }
  plume::RenderDescriptorSet *set = s.resolve_sets[cur][slot].get();
  ++s.resolve_set_used[cur];
  static std::atomic<u32> peak{0};
  if (s.resolve_set_used[cur] > peak.load(std::memory_order_relaxed)) {
    peak.store(s.resolve_set_used[cur], std::memory_order_relaxed);
    if ((s.resolve_set_used[cur] % 25) == 0)
      EOT_INFO("[resolve] {} blits in one frame (pool {})",
               s.resolve_set_used[cur], s.resolve_sets[cur].size());
  }

  plume::RenderFramebuffer *fb = GetFramebufferLocked(s, dest, nullptr);
  if (!fb)
    return refuse("destination cannot be a render target");

  const plume::RenderTextureBarrier to_blit[] = {
      plume::RenderTextureBarrier(src->texture,
                                  plume::RenderTextureLayout::SHADER_READ),
      plume::RenderTextureBarrier(dest->texture,
                                  plume::RenderTextureLayout::COLOR_WRITE)};
  rec.cmd->barriers(plume::RenderBarrierStage::GRAPHICS, to_blit, 2);
  src->layout = plume::RenderTextureLayout::SHADER_READ;
  dest->layout = plume::RenderTextureLayout::COLOR_WRITE;

  set->setTexture(0, src->texture, plume::RenderTextureLayout::SHADER_READ,
                  src->textureView.get());

  rec.cmd->setFramebuffer(fb);
  rec.cmd->setPipeline(pit->second.get());
  rec.cmd->setGraphicsPipelineLayout(s.blit_layout.get());
  rec.cmd->setGraphicsDescriptorSet(set, 0);
  rec.cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(dest->width),
                                              float(dest->height)));
  rec.cmd->setScissors(plume::RenderRect(0, 0, i32(dest->width),
                                         i32(dest->height)));
  rec.cmd->drawInstanced(3, 1, 0, 0);
  rec.cmd->setFramebuffer(nullptr);

  const plume::RenderTextureBarrier to_read(
      dest->texture, plume::RenderTextureLayout::SHADER_READ);
  rec.cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1);
  dest->layout = plume::RenderTextureLayout::SHADER_READ;

  return true;
}

}

namespace {
std::atomic<u32> g_resolves{0};
std::atomic<u32> g_no_dest{0};
std::atomic<u32> g_mismatch{0};
std::atomic<u32> g_blitted{0};
std::atomic<u32> g_no_src{0};
std::atomic<u32> g_mip_publishes{0};
std::atomic<u32> g_regional{0};
std::atomic<u32> g_self_copies{0};
std::atomic<u32> g_edram_fallbacks{0};
std::atomic<u32> g_exp_bias{0};
std::atomic<u32> g_front_probe{0};
}

void Video::LogResolveStats() {
  EOT_INFO("[resolve] {} copies, {} converting blits; skipped: {} no dest, "
           "{} no source, {} undescribable, {} self-copies; {} EDRAM fallbacks, {} with an exponent bias; {} into a mip above 0",
           g_resolves.load(), g_blitted.load(), g_no_dest.load(),
           g_no_src.load(), g_mismatch.load(), g_self_copies.load(),
           g_edram_fallbacks.load(), g_exp_bias.load(),
           g_mip_publishes.load());
}

void Video::ResolveRenderTarget(u32 flags, u32 dest_texture_va, u32 dest_level,
                                const ResolveRegion &region) {
  if (!dest_texture_va)
    return;
  plume::RenderFormat preferred = plume::RenderFormat::UNKNOWN;
  if (const AttachmentFormats bound = BoundAttachmentFormats();
      bound.color != plume::RenderFormat::UNKNOWN) {
    preferred = bound.color;
  }

  GuestTextureFetch dest_fetch;
  const bool have_dest_fetch =
      DecodeTextureFetchAt(dest_texture_va + kTextureObjectFetchOffset,
                           dest_fetch) &&
      dest_fetch.baseAddress != 0;

  GuestTexture *dest = ResolveGuestSurface(dest_texture_va);
  if (!dest) {
    GuestTextureFetch df;
    if (DecodeTextureFetchAt(dest_texture_va + kTextureObjectFetchOffset, df) &&
        df.baseAddress) {
      dest = FindOrBuildNativeTextureFromFetch(df, preferred);
    }
  }
  if (!dest)
    dest = FindOrBuildNativeTexture(dest_texture_va);

  if (!dest || !dest->texture) {
    g_no_dest.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (have_dest_fetch)
    PublishResolvedSurface(dest_fetch.baseAddress, dest);

  auto rec = AcquireRecordingList();
  if (!rec) {
    g_no_src.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto &s = state();
  const u32 slot = s.frame.load(std::memory_order_relaxed);

  const u32 source_index = flags & 0x7;
  const bool wants_depth = source_index == 4;
  GuestTexture *src = nullptr;
  if (wants_depth)
    src = rec.depthTarget;
  else if (source_index < kMaxRenderTargets)
    src = rec.colorTargets[source_index];

  const bool src_is_current = src && src->texture &&
                              src->drawnSerial == s.frame_serial;
  if (!src_is_current) {
    GuestTexture *fallback =
        wants_depth ? s.last_drawn_ds[slot]
                    : (s.busiest_rt ? s.busiest_rt : s.last_drawn_rt[slot]);
    if (fallback && (!fallback->texture || !fallback->hasContent)) {
      static std::atomic<u32> reject_probe{0};
      if (reject_probe.fetch_add(1, std::memory_order_relaxed) < 4) {
        EOT_INFO("[resolve] fallback rejected: cand={} {}x{} fmt={} "
                 "hasContent={} vs dest {}x{} fmt={}",
                 static_cast<const void *>(fallback), fallback->width,
                 fallback->height, static_cast<u32>(fallback->format),
                 fallback->hasContent, dest->width, dest->height,
                 static_cast<u32>(dest->format));
      }
      fallback = nullptr;
    } else if (!fallback) {
      static std::atomic<u32> null_probe{0};
      if (null_probe.fetch_add(1, std::memory_order_relaxed) < 2)
        EOT_INFO("[resolve] no last-drawn surface for slot {} yet", slot);
    }
    if (fallback) {
      src = fallback;
      g_edram_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (!src || !src->texture) {
    g_no_src.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (src == dest) {
    g_self_copies.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (src->format != dest->format || src->width != dest->width ||
      src->height != dest->height) {
    if (ResolveByBlitLocked(s, rec, src, dest)) {
      dest->hasContent = src->hasContent;
      if (g_blitted.fetch_add(1, std::memory_order_relaxed) == 0) {
        EOT_INFO("[resolve] first converting blit: fmt={} -> fmt={} ({}x{})",
                 static_cast<u32>(src->format), static_cast<u32>(dest->format),
                 dest->width, dest->height);
      }
      return;
    }
    if (g_mismatch.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_INFO("[resolve] {}x{} fmt={} -> {}x{} fmt={}: cannot convert",
               src->width, src->height, static_cast<u32>(src->format),
               dest->width, dest->height, static_cast<u32>(dest->format));
    }
    return;
  }
  const u32 level = std::min(dest_level, dest->mipLevels ? dest->mipLevels - 1
                                                         : 0u);
  u32 dst_w = std::max(dest->width >> level, 1u);
  u32 dst_h = std::max(dest->height >> level, 1u);

  u32 src_x = 0, src_y = 0, dst_x = 0, dst_y = 0;
  if (region.valid) {
    const u32 w = static_cast<u32>(region.right - region.left);
    const u32 h = static_cast<u32>(region.bottom - region.top);
    const bool fits_source = region.left >= 0 && region.top >= 0 &&
                             static_cast<u32>(region.right) <= src->width &&
                             static_cast<u32>(region.bottom) <= src->height;
    const bool fits_dest = region.destX >= 0 && region.destY >= 0 &&
                           static_cast<u32>(region.destX) + w <= dst_w &&
                           static_cast<u32>(region.destY) + h <= dst_h;
    if (w && h && fits_source && fits_dest) {
      src_x = static_cast<u32>(region.left);
      src_y = static_cast<u32>(region.top);
      dst_x = static_cast<u32>(region.destX);
      dst_y = static_cast<u32>(region.destY);
      dst_w = w;
      dst_h = h;
      if (g_regional.fetch_add(1, std::memory_order_relaxed) == 0) {
        EOT_INFO("[resolve] first placed resolve: {}x{} from ({},{}) to ({},{})",
                 dst_w, dst_h, src_x, src_y, dst_x, dst_y);
      }
    }
  }

  if (src->width < src_x + dst_w || src->height < src_y + dst_h) {
    if (g_mismatch.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_INFO("[resolve] source {}x{} smaller than destination level {} "
               "({}x{})",
               src->width, src->height, level, dst_w, dst_h);
    }
    return;
  }

  rec.cmd->setFramebuffer(nullptr);

  const plume::RenderTextureBarrier to_copy[] = {
      plume::RenderTextureBarrier(src->texture,
                                  plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(dest->texture,
                                  plume::RenderTextureLayout::COPY_DEST)};
  rec.cmd->barriers(plume::RenderBarrierStage::COPY, to_copy, 2);
  src->layout = plume::RenderTextureLayout::COPY_SOURCE;
  dest->layout = plume::RenderTextureLayout::COPY_DEST;

  if (REXCVAR_GET(eot_probe_resolve_green)) {
    if (plume::RenderFramebuffer *pfb = GetFramebufferLocked(s, dest, nullptr)) {
      const plume::RenderTextureBarrier to_write(
          dest->texture, plume::RenderTextureLayout::COLOR_WRITE);
      rec.cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_write, 1);
      dest->layout = plume::RenderTextureLayout::COLOR_WRITE;
      rec.cmd->setFramebuffer(pfb);
      rec.cmd->clearColor(0, plume::RenderColor(0.0f, 1.0f, 0.0f, 1.0f));
      rec.cmd->setFramebuffer(nullptr);
      const plume::RenderTextureBarrier to_read(
          dest->texture, plume::RenderTextureLayout::SHADER_READ);
      rec.cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1);
      dest->layout = plume::RenderTextureLayout::SHADER_READ;
      dest->hasContent = true;
      return;
    }
  }

  const plume::RenderBox src_box(i32(src_x), i32(src_y), i32(src_x + dst_w),
                                i32(src_y + dst_h));
  rec.cmd->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(dest->texture, level, 0),
      plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0), dst_x,
      dst_y, 0, &src_box);
  if (level != 0)
    g_mip_publishes.fetch_add(1, std::memory_order_relaxed);

  const plume::RenderTextureBarrier to_read(
      dest->texture, plume::RenderTextureLayout::SHADER_READ);
  rec.cmd->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1);
  dest->layout = plume::RenderTextureLayout::SHADER_READ;
  dest->hasContent = src->hasContent;

  if (g_identity_probe.fetch_add(1, std::memory_order_relaxed) < 3) {
    const GuestTexture *front = g_last_front.load(std::memory_order_relaxed);
    EOT_INFO("[resolve] src={} ({}x{} fmt={}) dest={} ({}x{} fmt={}) "
             "front={} src==dest:{} dest==front:{}",
             static_cast<const void *>(src), src->width, src->height,
             static_cast<u32>(src->format), static_cast<const void *>(dest),
             dest->width, dest->height, static_cast<u32>(dest->format),
             static_cast<const void *>(front), src == dest, dest == front);
  }

  if (dest == g_last_front.load(std::memory_order_relaxed)) {
    if (s.front_resolve_serial != s.frame_serial) {
      s.front_resolve_serial = s.frame_serial;
      s.front_resolves = 0;
      s.last_front_src = nullptr;
    }
    ++s.front_resolves;
    s.last_front_src = src;
  }

  if (dest == g_last_front.load(std::memory_order_relaxed) &&
      (g_front_probe.fetch_add(1, std::memory_order_relaxed) % 400) == 399) {
    EOT_INFO("[resolve] -> FRONT: flags=0x{:X} idx={} src={} {}x{} fmt={} "
             "drawnThisFrame={} hasContent={} fellBack={}",
             flags, source_index, static_cast<const void *>(src), src->width,
             src->height, static_cast<u32>(src->format),
             src->drawnSerial == s.frame_serial, src->hasContent,
             !src_is_current);
  }

  if (g_resolves.fetch_add(1, std::memory_order_relaxed) == 0)
    EOT_INFO("[resolve] first resolve: {}x{} fmt={} -> 0x{:08X}", src->width,
             src->height, static_cast<u32>(src->format), dest_texture_va);
}

void Video::ClearBoundTargets(u32 flags, u32 color_va, float z) {
  if (REXCVAR_GET(eot_protect_resolved))
    return;
  constexpr u32 kClearTarget = 0x1;
  constexpr u32 kClearDepth = 0x10;
  if (!(flags & (kClearTarget | kClearDepth)))
    return;
  if (BindDrawFramebuffer() != FramebufferBind::kBound)
    return;

  auto rec = AcquireRecordingList();
  if (!rec || !rec.framebuffer)
    return;

  plume::RenderTextureBarrier to_write[2];
  u32 barrier_count = 0;
  if (rec.colorTarget && rec.colorTarget->texture &&
      rec.colorTarget->layout != plume::RenderTextureLayout::COLOR_WRITE) {
    to_write[barrier_count++] = plume::RenderTextureBarrier(
        rec.colorTarget->texture, plume::RenderTextureLayout::COLOR_WRITE);
    rec.colorTarget->layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  if (rec.depthTarget && rec.depthTarget->texture &&
      rec.depthTarget->layout != plume::RenderTextureLayout::DEPTH_WRITE) {
    to_write[barrier_count++] = plume::RenderTextureBarrier(
        rec.depthTarget->texture, plume::RenderTextureLayout::DEPTH_WRITE);
    rec.depthTarget->layout = plume::RenderTextureLayout::DEPTH_WRITE;
  }
  if (barrier_count)
    rec.cmd->barriers(plume::RenderBarrierStage::GRAPHICS, to_write,
                      barrier_count);

  rec.cmd->setFramebuffer(rec.framebuffer);
  if ((flags & kClearTarget) && rec.colorTarget) {
    float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (color_va) {
      for (u32 i = 0; i < 4; ++i)
        rgba[i] = mem::try_load<float>(color_va + i * 4);
    }
    rec.cmd->clearColor(0, plume::RenderColor(rgba[0], rgba[1], rgba[2],
                                              rgba[3]));
  }
  if ((flags & kClearDepth) && rec.depthTarget) {
    rec.cmd->clearDepth(true, z);
  }
}

void Video::BeginGuestFrame() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.frame_present_committed = false;
  EnsureCommandListOpenLocked(s, s.frame.load(std::memory_order_relaxed));
}

Video::RecordingList Video::AcquireRecordingList() {
  auto &s = state();
  std::unique_lock lock(s.mutex);
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  if (!s.ready || DeviceIsLost() || !s.command_list_open[cur])
    return {};
  RecordingList out;
  out.cmd = s.command_lists[cur].get();
  out.framebuffer = s.bound_framebuffer;
  out.colorTarget = s.render_targets[0];
  out.depthTarget = s.depth_stencil;
  for (u32 i = 0; i < kMaxRenderTargets; ++i)
    out.colorTargets[i] = s.render_targets[i];
  const GuestTexture *target =
      s.render_targets[0] ? s.render_targets[0] : s.depth_stencil;
  if (target) {
    out.targetWidth = target->width;
    out.targetHeight = target->height;
  }
  out.lock = std::move(lock);
  return out;
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

void DrainValidationMessages() {
  auto *dev = static_cast<plume::D3D12Device *>(Video::HostDevice());
  if (!dev || !dev->d3d)
    return;
  ID3D12InfoQueue *queue = nullptr;
  if (FAILED(dev->d3d->QueryInterface(IID_PPV_ARGS(&queue))) || !queue)
    return;

  static bool break_disabled = false;
  if (!break_disabled) {
    break_disabled = true;
    queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
  }

  const u64 count = queue->GetNumStoredMessages();
  static u32 logged = 0;
  for (u64 i = 0; i < count && logged < 40; ++i) {
    SIZE_T len = 0;
    if (FAILED(queue->GetMessage(i, nullptr, &len)) || len == 0)
      continue;
    std::vector<u8> storage(len);
    auto *msg = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
    if (FAILED(queue->GetMessage(i, msg, &len)) || !msg->pDescription)
      continue;
    ++logged;
    EOT_ERROR("[d3d12] sev={} id={} {}", static_cast<u32>(msg->Severity),
              static_cast<u32>(msg->ID), msg->pDescription);
  }
  queue->ClearStoredMessages();
  queue->Release();
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
