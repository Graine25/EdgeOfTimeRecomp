#pragma once

#include <rex/types.h>

namespace eot::gpu {

void DrawGuestPrimitives(u32 device_va, u32 primitive_type, u32 start_vertex,
                         u32 vertex_count);

void QueueGuestUpDraw(u32 device_va, u32 primitive_type, u32 vertex_count, u32 stride,
                      u32 data_va);
void FlushPendingUpDraw();

void DrawGuestIndexedPrimitives(u32 device_va, u32 primitive_type, i32 base_vertex,
                                u32 start_index, u32 index_count);

void ClearGuestTargets(u32 device_va, u32 flags, u32 rect_va, u32 color_va, float z, u32 stencil);

void ResolveGuest(u32 device_va, u32 flags, u32 src_rect_va, u32 dest_texture_va,
                  u32 dest_point_va, u32 dest_level, u32 clear_color_va, float clear_z);

}
