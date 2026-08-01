#pragma once

#include "gpu/guest/vertex_declaration.h"

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/guest/resources.h"

namespace eot::gpu {

struct GuestShader;

const char *VertexUsageSemantic(VertexUsage usage);

const char *VertexUsageSemantic(VertexUsage usage);

struct VertexFetch {
  VertexUsage usage = VertexUsage::kPosition;
  u32 usageIndex = 0;
  bool miniFetch = false;
  u32 instructionAddress = 0;
  u32 parentAddress = 0;
};

inline constexpr u32 kMaxVertexFetches = 32;

struct VertexLayout {
  VertexFetch fetches[kMaxVertexFetches];
  u32 count = 0;
  bool truncated = false;
};

bool DecodeVertexLayout(const GuestShader *vs, VertexLayout &out);

struct InputElement {
  VertexUsage usage = VertexUsage::kPosition;
  u32 usageIndex = 0;
  u32 stream = 0;
  u32 offset = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
};

struct InputLayout {
  InputElement elements[kMaxVertexFetches];
  u32 count = 0;
  u32 recoveredStride = 0;
  bool incomplete = false;
  bool packedNormal = false;
};

u32 VertexFormatSize(plume::RenderFormat format);

bool BuildInputLayoutFromMicrocode(const GuestShader &shader,
                                   const VertexLayout &fetches,
                                   u32 buffer_stride, InputLayout &out);

bool BuildInputLayout(const VertexLayout &fetches, const VertexDeclaration &decl,
                      InputLayout &out);

void LogDeclTypeCensus();

void LogVertexLayoutStats();

}
