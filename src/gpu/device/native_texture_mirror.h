#pragma once

#include <rex/types.h>

#include "gpu/guest/resources.h"

namespace eot::gpu {

GuestTexture *FindOrBuildSurfaceMirror(u32 surface_va);

void RegisterSurfacePool(u32 record_va);

void EvictSurfaceMirror(u32 surface_va);

GuestTexture *FindOrBuildNativeTexture(u32 texture_va);

void LogNativeTextureStats();

GuestTexture *ResolveGuestSurface(u32 surface_va);

}
