#pragma once

#include <rex/types.h>

#include "gpu/guest/texture_fetch.h"

namespace eot::gpu::samplers {

u32 ResolveSlot(const GuestTextureFetch &fetch, bool force_clamp);

void LogStats();

void Shutdown();

}
