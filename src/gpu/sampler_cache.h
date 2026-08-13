#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

plume::RenderSamplerDesc DecodeSamplerFromFetch(const u32 fc[6]);

u32 ResolveSamplerSlotLocked(const plume::RenderSamplerDesc &desc);

}
