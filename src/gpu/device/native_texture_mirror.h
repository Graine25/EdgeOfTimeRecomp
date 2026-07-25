#pragma once

#include <rex/types.h>

#include "gpu/guest/resources.h"

namespace eot::gpu {

GuestTexture *FindOrBuildSurfaceMirror(u32 surface_va);

void RegisterSurfacePool(u32 record_va);

void EvictSurfaceMirror(u32 surface_va);

GuestTexture *FindOrBuildNativeTexture(u32 texture_va);

GuestTexture *FindOrBuildNativeTextureFromFetch(
    const struct GuestTextureFetch &fetch,
    plume::RenderFormat preferred_format = plume::RenderFormat::UNKNOWN);

void DrainEvictedNativeTextures(u32 slot);

void LogNativeTextureStats();

GuestTexture *ResolveMirrorByAddress(u32 address);

void PublishResolvedSurface(u32 base_address, GuestTexture *tex);

GuestTexture *ResolveGuestSurface(u32 surface_va);

void ScrubResolveLinks(GuestTexture *dead);

}
