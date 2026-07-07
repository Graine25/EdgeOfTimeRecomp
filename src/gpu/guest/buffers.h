#pragma once

#include <rex/types.h>

#include "gpu/guest/resources.h"

namespace eot::gpu {

GuestBuffer *RegisterBufferHeader(u32 header_va, ResourceType type);

GuestBuffer *ResolveGuestBuffer(u32 header_va, ResourceType type);

void LogBufferStats();

}
