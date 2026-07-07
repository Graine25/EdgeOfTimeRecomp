#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/guest/resources.h"

namespace eot::gpu {

u32 ShaderContainerOffset(ResourceType type);

bool IsValidShaderContainer(u32 container_va, ResourceType type);

u64 HashShaderContainer(u32 container_va, u32 physical_va = 0);

GuestShader *RegisterShaderObject(u32 object_va, ResourceType type,
                                  u32 container_va, u32 physical_va);

GuestShader *ResolveGuestShader(u32 object_va);

plume::RenderShader *GetOrLinkShader(GuestShader *gs, u32 specConstants);

const ShaderCacheEntry *FindShaderCacheEntry(u64 hash);

void LogShaderStats();

}
