#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/guest/d3d.h"

namespace eot::gpu {

struct DrawGeometry {
  plume::RenderVertexBufferView vertexViews[kMaxStreamSources];
  plume::RenderInputSlot vertexSlots[kMaxStreamSources];
  u32 vertexBufferCount = 0;

  plume::RenderIndexBufferView indexView;
  bool hasIndices = false;

  bool valid() const { return vertexBufferCount != 0; }
};

bool UploadDrawGeometry(const struct InputLayout &layout, u32 firstVertex,
                        u32 vertexCount, bool indexed, u32 startIndex,
                        u32 indexCount, bool windowSpace, u32 targetWidth,
                        u32 targetHeight, DrawGeometry &out);

void ResetGeometryFrame();

void LogGeometryUploadStats();

}
