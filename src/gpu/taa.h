#pragma once

#include <rex/types.h>

namespace eot::gpu {

struct VideoState;
struct GuestTexture;

namespace taa {

inline constexpr u32 kViewProjectionVa = 0x82496E3C;

bool IsSceneConsumer(u64 ps_hash);

void FrameJitter(const VideoState &s, float *jx, float *jy);
u32 JitterIndex(const VideoState &s);

void BeforeSceneConsumerDraw(VideoState &s, GuestTexture *const bound[16],
                             const float *camera_vp, u64 consumer_hash);
void EndFrame(VideoState &s);

void Reset(VideoState &s);
void Shutdown(VideoState &s);

}
}
