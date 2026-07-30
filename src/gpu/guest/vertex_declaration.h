#pragma once

#include <rex/types.h>

namespace eot::gpu {

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

bool CurrentVertexDeclaration(u32 device_va, VertexDeclaration &out);

u32 DeclarationsStamped();
u32 DeclarationsUnreadable();

void LogDeclTypeCensus();

}
