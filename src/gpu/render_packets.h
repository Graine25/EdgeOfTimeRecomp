#pragma once

#include <cstring>
#include <immintrin.h>

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

struct DeviceWindow {
  struct Block {
    u32 base, size;
  };
  static constexpr Block kBlocks[4] = {
      {dev::kFetchConstants, 16u * 24u},
      {dev::kVsBoolConstants, dev::kPsLoopConstants + 64 - dev::kVsBoolConstants},
      {dev::kSurfaceInfo, dev::kPolyOffsetBackOffset + 4 - dev::kSurfaceInfo},
      {dev::kTextureObject0, 16u * 4u},
  };
  struct Pages {
    bool in[kDeviceSnapshotBytes >> kDevicePageShift] = {};
    constexpr Pages() {
      for (const Block &b : kBlocks)
        for (u32 p = b.base >> kDevicePageShift; p < (b.base + b.size) >> kDevicePageShift; ++p)
          in[p] = true;
    }
  };
  static const bool *PageTable() {
    static constexpr Pages pages{};
    return pages.in;
  }
  alignas(64) u8 image[kDeviceSnapshotBytes];
  template <u32 I> void CopyBlock(const u8 *regs) {
    std::memcpy(image + kBlocks[I].base, regs + kBlocks[I].base, kBlocks[I].size);
  }
  void Capture(const u8 *regs) {
    CopyBlock<0>(regs);
    CopyBlock<1>(regs);
    CopyBlock<2>(regs);
    CopyBlock<3>(regs);
  }
};
static_assert((DeviceWindow::kBlocks[0].base | DeviceWindow::kBlocks[0].size |
               DeviceWindow::kBlocks[1].base | DeviceWindow::kBlocks[1].size |
               DeviceWindow::kBlocks[2].base | DeviceWindow::kBlocks[2].size |
               DeviceWindow::kBlocks[3].base | DeviceWindow::kBlocks[3].size) % 32 == 0);
static_assert(DeviceWindow::kBlocks[3].base + DeviceWindow::kBlocks[3].size <= kDeviceSnapshotBytes);

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
  DeviceWindow window;
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
  bool refresh = false;
};

}
