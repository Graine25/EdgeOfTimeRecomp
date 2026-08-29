#include <mutex>

#include <plume_render_interface.h>
#include <cstring>
#include <memory>
#include <vector>

#include "core/logging.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/format.h"

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

namespace eot::gpu {

namespace {

constexpr u64 kChunkSize = 32ull * 1024 * 1024;

struct Chunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *cpu = nullptr;
  u64 capacity = 0;
  u64 used = 0;
};

struct Ring {
  std::vector<Chunk> chunks[kNumFrames];
  u64 frame_bytes[kNumFrames] = {};
};

Ring &ring() {
  static Ring r;
  return r;
}

bool MakeChunk(Chunk &chunk, u64 size) {
  auto &s = state();
  plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(size);
  desc.flags = plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX |
               plume::RenderBufferFlag::CONSTANT;
  chunk.buffer = CreateHostBuffer(s.device.get(), desc, "upload-ring");
  if (!chunk.buffer)
    return false;
  chunk.cpu = static_cast<u8 *>(chunk.buffer->map());
  if (!chunk.cpu) {
    EOT_ERROR("upload ring: map() failed for a {} byte chunk", size);
    chunk.buffer.reset();
    return false;
  }
  chunk.capacity = size;
  chunk.used = 0;
  return true;
}

}

bool UploadRingInit() {
  auto &r = ring();
  for (u32 i = 0; i < kNumFrames; ++i) {
    r.chunks[i].clear();
    Chunk c;
    if (!MakeChunk(c, kChunkSize))
      return false;
    r.chunks[i].push_back(std::move(c));
  }
  return true;
}

static u64 g_ring_epoch = 0;
u64 UploadRingEpoch() { return g_ring_epoch; }

void UploadRingResetFrame(u32 slot) {
  auto &r = ring();
  ++g_ring_epoch;
  auto &chunks = r.chunks[slot];
  for (size_t i = 0; i < chunks.size(); ++i)
    chunks[i].used = 0;
  while (chunks.size() > 4) {
    chunks.back().buffer->unmap();
    chunks.pop_back();
  }
  r.frame_bytes[slot] = 0;
}

bool UploadAllocate(u64 size, u64 alignment, UploadAlloc *out) {
  *out = UploadAlloc{};
  if (size == 0)
    return false;
  auto &s = state();
  auto &r = ring();
  auto &chunks = r.chunks[s.recording_slot()];
  if (alignment == 0)
    alignment = 1;
  for (auto &c : chunks) {
    const u64 start = (c.used + alignment - 1) / alignment * alignment;
    if (start + size <= c.capacity) {
      out->buffer = c.buffer.get();
      out->offset = start;
      out->cpu = c.cpu + start;
      out->size = size;
      c.used = start + size;
      r.frame_bytes[s.recording_slot()] += size;
      return true;
    }
  }
  Chunk c;
  const u64 want = size + alignment > kChunkSize ? size + alignment : kChunkSize;
  if (!MakeChunk(c, want))
    return false;
  chunks.push_back(std::move(c));
  return UploadAllocate(size, alignment, out);
}

bool UploadBytes(const void *src, u64 size, u64 alignment, UploadAlloc *out) {
  if (!UploadAllocate(size, alignment, out))
    return false;
  std::memcpy(out->cpu, src, size);
  return true;
}

u64 UploadRingBytesThisFrame() {
  auto &s = state();
  return ring().frame_bytes[s.recording_slot()];
}

}

namespace eot::gpu {

u32 AllocateDescriptorSlot(VideoState &s) {
  for (size_t i = kNullTextureDescriptorCount; i < s.descriptor_slot_used.size(); ++i) {
    if (!s.descriptor_slot_used[i]) {
      s.descriptor_slot_used[i] = true;
      return static_cast<u32>(i);
    }
  }
  return kInvalidDescriptorIndex;
}

void DrainDescriptorSlotsLocked(VideoState &s, u32 slot) {
  for (const auto &d : s.descriptor_graveyard[slot]) {
    if (s.texture_descriptor_set) {
      s.texture_descriptor_set->setTexture(d.slot, s.null_textures[d.null_index].get(),
                                           plume::RenderTextureLayout::SHADER_READ,
                                           s.null_texture_views[d.null_index].get());
    }
    if (d.slot >= kNullTextureDescriptorCount && d.slot < s.descriptor_slot_used.size())
      s.descriptor_slot_used[d.slot] = false;
  }
  s.descriptor_graveyard[slot].clear();
}

namespace {

u32 NullIndexFor(const HostTexture &host) {
  if (host.viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D)
    return kNullTexture3DDescriptorIndex;
  if (host.viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE)
    return kNullTextureCubeDescriptorIndex;
  return kNullTexture2DDescriptorIndex;
}

plume::RenderTextureViewDesc SamplingViewDesc(const HostTexture &host) {
  plume::RenderTextureViewDesc view_desc;
  view_desc.format = host.viewFormat != plume::RenderFormat::UNKNOWN
                         ? host.viewFormat
                         : SampledViewFormat(host.format);
  view_desc.dimension = host.viewDimension;
  view_desc.mipLevels = host.mipLevels ? host.mipLevels : 1;
  return view_desc;
}

plume::RenderSwizzle ToHostSwizzle(u32 source) {
  switch (source & 7) {
  case 0:
    return plume::RenderSwizzle::R;
  case 1:
    return plume::RenderSwizzle::G;
  case 2:
    return plume::RenderSwizzle::B;
  case 3:
    return plume::RenderSwizzle::A;
  case 5:
    return plume::RenderSwizzle::ONE;
  default:
    return plume::RenderSwizzle::ZERO;
  }
}

u32 PublishView(VideoState &s, HostTexture &host, plume::RenderTextureView *view) {
  const u32 slot = AllocateDescriptorSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    EOT_ERROR("bindless texture heap full at {} slots", kBindlessTextureCount);
    return kInvalidDescriptorIndex;
  }
  s.texture_descriptor_set->setTexture(slot, host.texture.get(),
                                       plume::RenderTextureLayout::SHADER_READ, view);
  return slot;
}

}

void ReleaseTextureSRVLocked(VideoState &s, HostTexture &host) {
  const u32 null_index = NullIndexFor(host);
  s.texture_generation.fetch_add(1, std::memory_order_relaxed);
  if (host.descriptorIndex != kInvalidDescriptorIndex) {
    s.descriptor_graveyard[s.recording_slot()].push_back({host.descriptorIndex, null_index});
    host.descriptorIndex = kInvalidDescriptorIndex;
  }
  for (auto &[swizzle, srv] : host.swizzledSrvs) {
    if (srv.descriptorIndex != kInvalidDescriptorIndex)
      s.descriptor_graveyard[s.recording_slot()].push_back({srv.descriptorIndex, null_index});
    if (srv.view)
      ParkView(s, std::move(srv.view));
  }
  host.swizzledSrvs.clear();
}

u32 BindTextureSRVLocked(VideoState &s, HostTexture &host) {
  if (!host.texture || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  if (host.descriptorIndex != kInvalidDescriptorIndex)
    return host.descriptorIndex;
  if (!host.srv)
    host.srv = host.texture->createTextureView(SamplingViewDesc(host));
  if (!host.srv)
    return kInvalidDescriptorIndex;
  host.descriptorIndex = PublishView(s, host, host.srv.get());
  return host.descriptorIndex;
}

u32 BindTextureSRVSwizzledLocked(VideoState &s, HostTexture &host, u32 swizzle) {
  swizzle &= 0xFFF;
  if (swizzle == kIdentityFetchSwizzle)
    return BindTextureSRVLocked(s, host);
  if (!host.texture || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  auto &entry = host.swizzledSrvs[swizzle];
  if (entry.descriptorIndex != kInvalidDescriptorIndex)
    return entry.descriptorIndex;
  if (!entry.view) {
    plume::RenderTextureViewDesc view_desc = SamplingViewDesc(host);
    view_desc.componentMapping = plume::RenderComponentMapping(
        ToHostSwizzle(swizzle), ToHostSwizzle(swizzle >> 3), ToHostSwizzle(swizzle >> 6),
        ToHostSwizzle(swizzle >> 9));
    entry.view = host.texture->createTextureView(view_desc);
  }
  if (!entry.view)
    return kInvalidDescriptorIndex;
  entry.descriptorIndex = PublishView(s, host, entry.view.get());
  return entry.descriptorIndex;
}

}
