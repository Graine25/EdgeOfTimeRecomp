#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

namespace eot::gpu {

plume::RenderSamplerDesc DecodeSamplerFromFetch(const u32 fc[6], bool mipmapped_upload = true,
                                                bool synthesized_mips = false);

u32 ResolveSamplerSlotLocked(const plume::RenderSamplerDesc &desc);

}
