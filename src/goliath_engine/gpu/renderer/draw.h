#pragma once

#include <cstdint>

namespace plume {
struct RenderCommandList;
}

namespace eot::gpu {

void ReplayCapturedDraws(plume::RenderCommandList* cmd, uint32_t backbufferW,
                         uint32_t backbufferH);

}
