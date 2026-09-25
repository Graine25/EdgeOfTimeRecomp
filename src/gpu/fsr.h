#pragma once

#include <rex/types.h>

namespace eot::gpu {

struct VideoState;
struct HostTexture;

namespace fsr {

inline constexpr float kSharpness = 0.5f;

HostTexture *EnsureTarget(VideoState &s, u32 width, u32 height);

void Shutdown(VideoState &s);

}
}
