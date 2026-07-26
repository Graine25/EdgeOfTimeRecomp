#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/guest/resources.h"

namespace eot::gpu {

struct GuestShader;

enum class VertexUsage : u32 {
  kPosition = 0,
  kBlendWeight = 1,
  kBlendIndices = 2,
  kNormal = 3,
  kPointSize = 4,
  kTexCoord = 5,
  kTangent = 6,
  kBinormal = 7,
  kTessFactor = 8,
  kPositionT = 9,
  kColor = 10,
  kFog = 11,
  kDepth = 12,
  kSample = 13,
};

const char *VertexUsageName(VertexUsage usage);

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

struct VertexDeclElement {
  u32 stream = 0;
  u32 offset = 0;
  u32 declType = 0;
  VertexUsage usage = VertexUsage::kPosition;
  u32 usageIndex = 0;
};

inline constexpr u32 kMaxDeclElements = 32;

struct VertexDeclaration {
  VertexDeclElement elements[kMaxDeclElements];
  u32 count = 0;
  u32 maxStream = 0;
};

bool DecodeVertexDeclaration(u32 decl_va, VertexDeclaration &out);

void RegisterVertexDeclaration(u32 decl_va);

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
  bool incomplete = false;
  bool packedNormal = false;
};

u32 VertexFormatSize(plume::RenderFormat format);

bool BuildInputLayoutFromMicrocode(const GuestShader &shader,
                                   const VertexLayout &fetches,
                                   u32 buffer_stride, InputLayout &out);

bool BuildInputLayout(const VertexLayout &fetches, const VertexDeclaration &decl,
                      InputLayout &out);

bool CurrentVertexDeclaration(u32 device_va, VertexDeclaration &out);

void LogDeclTypeCensus();

void LogVertexLayoutStats();

}
