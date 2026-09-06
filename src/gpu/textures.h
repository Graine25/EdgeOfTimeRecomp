#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/resources.h"

namespace eot::gpu {

struct VideoState;

GuestTexture *GetGuestTexture(VideoState &s, u32 header_va, bool create_host_image = true);

u32 PrepareTextureForSampling(VideoState &s, GuestTexture &t,
                              u32 swizzle = kIdentityFetchSwizzle);

bool EnsureResolveMirror(VideoState &s, GuestTexture &t, bool depth_source, float scale = 0.0f);

plume::RenderFramebuffer *GetMipFramebuffer(VideoState &s, GuestTexture &t, u32 mip);

void NotifyResourceUnlocked(u32 resource_va);
u64 ResourceUnlockSeq(u32 resource_va);

}

namespace eot::gpu {

plume::RenderSamplerDesc DecodeSamplerFromFetch(const u32 fc[6]);

u32 ResolveSamplerSlotLocked(const plume::RenderSamplerDesc &desc);

}
