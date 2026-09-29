#pragma once

namespace plume {
struct RenderBuffer;
struct RenderTexture;
struct RenderTextureDesc;
}

namespace eot::gpu {

void TagHostAllocation(plume::RenderTexture *texture, const char *tag, const plume::RenderTextureDesc &desc);
void TagHostAllocation(plume::RenderBuffer *buffer, const char *tag);

void MemoryReportTick();

}
