#include <bit>
#include <cmath>
#include <cstdint>

#include <rex/hook.h>

#include "core/memory_helpers.h"

namespace {

constexpr uint32_t kMovePos = 48;
constexpr uint32_t kMoveRot = 60;
constexpr uint32_t kMoveScale = 76;
constexpr uint32_t kFlags = 80;
constexpr uint32_t kFlagAccumulated = 0x40;

float LoadF(uint32_t at) { return std::bit_cast<float>(eot::mem::load<uint32_t>(at)); }
void StoreF(uint32_t at, float v) { eot::mem::store<uint32_t>(at, std::bit_cast<uint32_t>(v)); }

struct Quat {
  float x, y, z, w;
};

Quat Load(uint32_t at) { return {LoadF(at), LoadF(at + 4), LoadF(at + 8), LoadF(at + 12)}; }

Quat ScaleAngle(Quat q, float scale) {
  if (q.w < 0.0f)
    q = {-q.x, -q.y, -q.z, -q.w};
  const float v = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  if (!(v > 1e-12f))
    return {0.0f, 0.0f, 0.0f, 1.0f};
  const float half = std::atan2(v, q.w) * scale;
  const float s = std::sin(half) / v;
  return {q.x * s, q.y * s, q.z * s, std::cos(half)};
}

Quat Multiply(const Quat &a, const Quat &b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
          a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

}

REX_HOOK_RAW(eot_ANAnimTree_AccumulateMove) {
  const uint32_t tree = ctx.r3.u32;
  const uint32_t position = ctx.r4.u32;
  const uint32_t rotation = ctx.r5.u32;
  const float scale = LoadF(tree + kMoveScale);
  eot::mem::store<uint32_t>(tree + kFlags, eot::mem::load<uint32_t>(tree + kFlags) | kFlagAccumulated);

  Quat sum = Multiply(Load(tree + kMoveRot), ScaleAngle(Load(rotation), scale));
  const float len = std::sqrt(sum.x * sum.x + sum.y * sum.y + sum.z * sum.z + sum.w * sum.w);
  if (len > 1e-12f)
    sum = {sum.x / len, sum.y / len, sum.z / len, sum.w / len};
  else
    sum = {0.0f, 0.0f, 0.0f, 1.0f};
  StoreF(tree + kMoveRot, sum.x);
  StoreF(tree + kMoveRot + 4, sum.y);
  StoreF(tree + kMoveRot + 8, sum.z);
  StoreF(tree + kMoveRot + 12, sum.w);

  for (uint32_t k = 0; k < 3; ++k)
    StoreF(tree + kMovePos + 4 * k, LoadF(tree + kMovePos + 4 * k) + LoadF(position + 4 * k) * scale);
}
