#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/format.h"

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
