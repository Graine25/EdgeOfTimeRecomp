#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/types.h>

#include "gpu/pipeline/pso_records.h"

namespace eot::gpu {

enum class PsoBias : u8 { None = 0, Material = 1, Shadow = 2, Literal = 3 };

struct PsoShadowValue {
  i32 depthBias = 0;
  f32 slope = 0.0f;
  bool operator==(const PsoShadowValue &o) const { return depthBias == o.depthBias && slope == o.slope; }
};

struct PsoSetRow {
  u64 vsHash = 0, psHash = 0;
  u32 spec = 0;
  u16 core = 0, decl = 0, strides = 0;
  PsoBias bias = PsoBias::None;
  bool derived = false;
  u8 msaa = 0;
  i32 vk = -1, pk = -1;
  f32 m = 0.0f;
  u32 literal = 0;
};

struct PsoSetPackage {
  std::string name;
  std::vector<u16> parents, children;
  bool level = false;
  std::vector<u32> rows;
  std::vector<PsoShadowValue> shadow;
};

struct PsoSetKey {
  bool pixel = false;
  u64 key = 0;
  std::vector<u64> hashes;
};

struct PsoHashPair {
  u64 vs = 0, ps = 0;
  u32 spec = 0;
};

constexpr u32 kPsoSpecEither = 0xFF;

struct PsoSet {
  std::vector<PipelineState> cores;
  std::vector<i32> velocityOf;
  std::vector<std::vector<u8>> decls;
  std::vector<std::array<u32, 16>> strides;
  std::vector<PsoShadowValue> literals;
  std::vector<PsoSetRow> rows;
  std::vector<u32> boot;
  std::unordered_map<u16, PsoSetPackage> packages;
  std::vector<PsoSetKey> keys;
  std::unordered_map<u64, u32> vsKeyIndex, psKeyIndex;
  std::vector<std::vector<u32>> keyRows;
  std::vector<std::vector<u16>> rowOwners;
  u32 captured = 0, derived = 0, bad = 0;
};

const PsoSet &CompiledInSet();

bool PsoSetRecord(const PsoSet &set, const PsoSetRow &row, const PsoShadowValue *shadow,
                  PsoRecord *out, i32 core = -1, const PsoHashPair *hashes = nullptr);

}
