#pragma once

#include <cstdint>

namespace plume {
struct RenderDevice;
}

namespace eot::gpu {

plume::RenderDevice* Device();

bool VideoInit(void* native_window_handle, uint32_t width, uint32_t height);

bool VideoIsInitialized();

void VideoPresent();

void VideoSetClearColor(float r, float g, float b, float a);

void VideoShutdown();

}
