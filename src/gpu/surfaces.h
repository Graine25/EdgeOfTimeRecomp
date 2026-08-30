#pragma once

#include <rex/types.h>

#include "gpu/resources.h"

namespace eot::gpu {

struct VideoState;

GuestSurface *GetGuestSurface(VideoState &s, u32 surface_va);

plume::RenderFormat SurfaceHostFormat(const GuestSurface &surface);

plume::RenderFramebuffer *GetFramebuffer(VideoState &s, HostTexture *const color[4],
                                         u32 color_count, HostTexture *depth);

}
