#pragma once

#include <cstdint>

namespace plume {
struct RenderCommandList;
struct RenderDescriptorSet;
struct RenderPipelineLayout;
}

namespace eot::gpu {

bool EnsureTextureSystem();

void BeginTextureFrame();

uint32_t GetOrCreateTextureIndex(plume::RenderCommandList* cmd, uint32_t guestAddr, uint32_t d0,
                                 uint32_t d1, uint32_t d2);

plume::RenderDescriptorSet* TextureSet();
plume::RenderDescriptorSet* SamplerSet();
plume::RenderPipelineLayout* TexturedPipelineLayout();

plume::RenderPipelineLayout* GameLayout();
plume::RenderDescriptorSet* Tex3DSet();
plume::RenderDescriptorSet* TexCubeSet();
plume::RenderDescriptorSet* Tex1DSet();
void GameCbvIndices(uint32_t& vs, uint32_t& ps, uint32_t& shared);

bool GetBoundTexture(uint32_t sampler, uint32_t& addr, uint32_t& d0, uint32_t& d1, uint32_t& d2);

}
