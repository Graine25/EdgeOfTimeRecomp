#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"

namespace eot::gpu {

struct GuestShader;
struct InputLayout;

struct CachedIndexRange {
  plume::RenderBuffer *buffer = nullptr;
  u64 offset = 0;
  u32 count = 0;
  u32 lo = 0, hi = 0;
  bool is32 = false;
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  u64 lastUseFrame = 0;
};

struct TargetWords {
  u32 colorVa[4] = {};
  u32 colorWords[4][5] = {};
  u32 colorInfo[4] = {};
  u32 colorCount = 0;
  u32 depthVa = 0;
  u32 depthWords[5] = {};
};

struct DrawPacket {
  u32 device_va = 0;
  u32 prim = 0;
  bool indexed = false;
  bool rectList = false;
  plume::RenderPrimitiveTopology topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  u32 vertexCount = 0;
  GuestShader *vs = nullptr;
  GuestShader *ps = nullptr;
  u32 vs_va = 0, ps_va = 0;
  const InputLayout *layout = nullptr;
  TargetWords targets;
  bool hasCached = false;
  CachedIndexRange cached;
  UploadAlloc index_alloc;
  u32 index_count = 0;
  i32 host_base_vertex = 0;
  u32 max_slot = 0;
  u32 strides[16] = {};
  plume::RenderVertexBufferView views[16];
  plume::RenderInputSlot slots[16];
  UploadAlloc zero;
  UploadAlloc vs_consts, ps_consts;
  alignas(16) u8 window[kDeviceSnapshotBytes];
};

struct ClearPacket {
  u32 device_va = 0;
  u32 flags = 0;
  bool hasRect = false;
  i32 rect[4] = {};
  float rgba[4] = {};
  float z = 0.0f;
  u32 stencil = 0;
  TargetWords targets;
};

struct ResolvePacket {
  u32 device_va = 0;
  u32 flags = 0;
  u32 srcVa = 0;
  u32 srcWords[5] = {};
  u32 destVa = 0;
  u32 destLevel = 0;
  bool hasRect = false;
  i32 rect[4] = {};
  bool hasPoint = false;
  i32 point[2] = {};
  bool hasColor = false;
  float rgba[4] = {};
  float clearZ = 0.0f;
  u32 dsVa = 0;
  u32 dsWords[5] = {};
};

}
