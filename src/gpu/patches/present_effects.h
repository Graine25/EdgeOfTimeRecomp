#pragma once

#include <rex/types.h>

namespace eot::gpu {

struct VideoState;
struct HostTexture;

u32 ApplyPresentEffects(VideoState &s, HostTexture &front, u32 front_srv);

void SelectPresentBlitMode(u32 src_w, u32 src_h, float dst_w, float dst_h, float extra[4]);

}
