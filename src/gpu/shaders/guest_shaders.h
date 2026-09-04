#pragma once

#include <vector>

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/resources.h"

struct ShaderCacheEntry;

namespace eot::gpu {

struct VideoState;

constexpr u32 kSpecR11G11B10Normal = 1u << 0;
constexpr u32 kSpecAlphaTest = 1u << 1;
constexpr u32 kSpecSintTexcoord = 1u << 2;

bool GuestShadersInit();
u32 GuestShaderCacheCount();

GuestShader *RegisterGuestShader(VideoState &s, u32 object_va, bool is_pixel);
GuestShader *FindGuestShader(VideoState &s, u32 object_va);

const ShaderCacheEntry *FindShaderCacheEntry(u64 hash);
u64 CanonicalShaderHash(u64 hash);
void VertexInputsFromEntry(const ShaderCacheEntry &e, std::vector<VertexInput> &out);
plume::RenderShader *GetHostShaderByHash(VideoState &s, u64 hash, u32 spec_mask, bool is_pixel,
                                         bool worker = false);

plume::RenderShader *ResolveHostShader(VideoState &s, GuestShader &shader, u32 spec_mask);

}
