#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/guest/d3d.h"

namespace eot::gpu {

enum GuestPrimitiveType : u32 {
  kPrimPointList = 1,
  kPrimLineList = 2,
  kPrimLineStrip = 3,
  kPrimTriangleList = 4,
  kPrimTriangleFan = 5,
  kPrimTriangleStrip = 6,
  kPrimQuadList = 13,
};

struct DrawGeometry {
  plume::RenderVertexBufferView vertexViews[kMaxStreamSources];
  plume::RenderInputSlot vertexSlots[kMaxStreamSources];
  u32 vertexBufferCount = 0;

  plume::RenderIndexBufferView indexView;
  bool hasIndices = false;
  u32 indexCount = 0;

  bool valid() const { return vertexBufferCount != 0; }
};

bool UploadDrawGeometry(const struct InputLayout &layout, u32 firstVertex,
                        u32 vertexCount, bool indexed, u32 startIndex,
                        u32 indexCount, u32 baseVertexIndex, u32 primitiveType,
                        bool windowSpace, u32 targetWidth, u32 targetHeight,
                        DrawGeometry &out);

void ResetGeometryFrame();

void LogGeometryUploadStats();

}
