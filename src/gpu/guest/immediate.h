#pragma once

#include <rex/types.h>

namespace eot::gpu {

void NoteImmediateVertices(u32 device_va, u32 primitiveType, u32 vertexCount,
                           u32 stride, u32 address);

void FlushImmediateVertices();

void LogImmediateStats();

}
