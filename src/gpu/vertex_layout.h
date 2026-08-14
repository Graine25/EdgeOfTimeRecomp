#pragma once

#include <vector>

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/resources.h"

namespace eot::gpu {

struct VideoState;

constexpr u32 kSyntheticVertexSlot = 15;

struct InputLayout {
  u64 key = 0;
  std::vector<plume::RenderInputElement> elements;
  u32 streamMask = 0;
  bool needsSyntheticSlot = false;
  u32 swappedTexcoords = 0;
  u32 swappedNormals = 0;
  u32 swappedBinormals = 0;
  u32 swappedTangents = 0;
  u32 swappedBlendWeights = 0;
  u32 swappedPositions = 0;
  u32 sintTexcoords = 0;
  u32 packedDec3 = 0;
  bool anyPacked111110 = false;
  u32 spec = 0;
  u32 streamExtent[16] = {};
};

const InputLayout *GetInputLayout(VideoState &s, GuestShader &vs, u32 declaration_va);

}
