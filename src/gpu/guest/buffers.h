#pragma once

#include <rex/types.h>

#include "gpu/guest/resources.h"

namespace eot::gpu {

GuestBuffer *RegisterBufferHeader(u32 header_va, ResourceType type);

void NotifyBufferAddressFixup(u32 resource_va, u32 base_va);

GuestBuffer *ResolveGuestBuffer(u32 header_va, ResourceType type);

bool ReadStreamFetch(u32 device_va, u32 stream, u32 &addr, u32 &size);

void LogBufferStats();
void LogDrawStats();

}
