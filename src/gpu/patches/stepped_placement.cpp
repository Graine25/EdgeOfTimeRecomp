#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_PhysicsWorld_Update);
REX_EXTERN(__imp__eot_GOGameObj_SetLocalMatrix);

namespace {

constexpr uint32_t kUpdateWorldMatrix = 0x820EFE88;

constexpr uint32_t kPhysicsWorld = 0x824A11B8;
constexpr uint32_t kWorldStep = kPhysicsWorld + 1780;
constexpr uint32_t kWorldTotalSteps = kPhysicsWorld + 1792;
constexpr uint32_t kWorldAccumulator = kPhysicsWorld + 1800;

constexpr uint32_t kLocal = 96;
constexpr uint32_t kRowPosition = 3;
constexpr uint32_t kRigidBody = 24;

constexpr float kMaxStep = 2.0f;
constexpr float kMinCosTurn = 0.5f;

struct Pose {
  float row[4][3];
};

struct Entry {
  uint32_t object = 0;
  uint32_t vtable = 0;
  uint32_t body = 0;
  uint32_t step = 0;
  bool has_previous = false;
  Pose previous;
  Pose last;
};

constexpr size_t kMaxEntries = 16;
std::array<Entry, kMaxEntries> g_entries;
std::mutex g_mutex;
std::atomic<bool> g_in_update{false};

float LoadF(uint32_t at) { return std::bit_cast<float>(eot::mem::load<uint32_t>(at)); }
void StoreF(uint32_t at, float v) { eot::mem::store<uint32_t>(at, std::bit_cast<uint32_t>(v)); }

Pose ReadPose(uint32_t object) {
  Pose p;
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t k = 0; k < 3; ++k)
      p.row[r][k] = LoadF(object + kLocal + (r * 4 + k) * 4);
  return p;
}

void WritePose(uint32_t object, const Pose &p) {
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t k = 0; k < 3; ++k)
      StoreF(object + kLocal + (r * 4 + k) * 4, p.row[r][k]);
}

float Dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Length(const float *a) { return std::sqrt(Dot(a, a)); }

bool Continuous(const Pose &a, const Pose &b) {
  float d[3];
  for (uint32_t k = 0; k < 3; ++k)
    d[k] = b.row[kRowPosition][k] - a.row[kRowPosition][k];
  if (Length(d) > kMaxStep)
    return false;
  for (uint32_t r = 0; r < 3; ++r) {
    const float la = Length(a.row[r]), lb = Length(b.row[r]);
    if (la <= 0.0f || lb <= 0.0f || Dot(a.row[r], b.row[r]) < kMinCosTurn * la * lb)
      return false;
  }
  return true;
}

Pose Blend(const Pose &a, const Pose &b, float t) {
  Pose out = b;
  for (uint32_t k = 0; k < 3; ++k)
    out.row[kRowPosition][k] = a.row[kRowPosition][k] + (b.row[kRowPosition][k] - a.row[kRowPosition][k]) * t;
  float x[3], y[3], z[3];
  for (uint32_t k = 0; k < 3; ++k) {
    x[k] = a.row[0][k] + (b.row[0][k] - a.row[0][k]) * t;
    y[k] = a.row[1][k] + (b.row[1][k] - a.row[1][k]) * t;
  }
  const float lx = Length(x);
  if (lx <= 0.0f)
    return out;
  for (float &v : x)
    v /= lx;
  const float yx = Dot(y, x);
  for (uint32_t k = 0; k < 3; ++k)
    y[k] -= yx * x[k];
  const float ly = Length(y);
  if (ly <= 0.0f)
    return out;
  for (float &v : y)
    v /= ly;
  z[0] = x[1] * y[2] - x[2] * y[1];
  z[1] = x[2] * y[0] - x[0] * y[2];
  z[2] = x[0] * y[1] - x[1] * y[0];
  if (Dot(z, b.row[2]) < 0.0f)
    for (float &v : z)
      v = -v;
  const float sx = Length(b.row[0]), sy = Length(b.row[1]), sz = Length(b.row[2]);
  for (uint32_t k = 0; k < 3; ++k) {
    out.row[0][k] = x[k] * sx;
    out.row[1][k] = y[k] * sy;
    out.row[2][k] = z[k] * sz;
  }
  return out;
}

bool Alive(const Entry &e) {
  return eot::mem::load<uint32_t>(e.object) == e.vtable && eot::mem::load<uint32_t>(e.object + kRigidBody) == e.body;
}

void UpdateWorldMatrix(const PPCContext &ctx, uint8_t *base, uint32_t object) {
  PPCFunc *fn = rex::runtime::ResolveIndirectFunction(kUpdateWorldMatrix);
  if (!fn)
    return;
  PPCContext call = ctx;
  call.r3.u32 = object;
  fn(call, base);
}

Entry *Find(uint32_t object) {
  for (Entry &e : g_entries)
    if (e.object == object)
      return &e;
  return nullptr;
}

Entry *Claim(uint32_t object) {
  if (Entry *e = Find(object))
    return e;
  Entry *pick = &g_entries[0];
  for (Entry &e : g_entries) {
    if (!e.object)
      return &e;
    if (e.step < pick->step)
      pick = &e;
  }
  return pick;
}

uint64_t g_placements = 0, g_drawn_between = 0;

}

REX_HOOK_RAW(eot_GOGameObj_SetLocalMatrix) {
  const uint32_t object = ctx.r3.u32;
  __imp__eot_GOGameObj_SetLocalMatrix(ctx, base);
  if (!object)
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_in_update.load(std::memory_order_acquire)) {
    if (Entry *e = Find(object))
      *e = Entry{};
    return;
  }
  const uint32_t step = eot::mem::load<uint32_t>(kWorldTotalSteps);
  Entry *e = Claim(object);
  const Pose now = ReadPose(object);
  if (e->object == object && e->step + 1 == step) {
    e->previous = e->last;
    e->has_previous = Continuous(e->previous, now);
  } else if (!(e->object == object && e->step == step)) {
    e->has_previous = false;
  }
  e->object = object;
  e->vtable = eot::mem::load<uint32_t>(object);
  e->body = eot::mem::load<uint32_t>(object + kRigidBody);
  e->step = step;
  e->last = now;
  ++g_placements;
}

REX_HOOK_RAW(eot_PhysicsWorld_Update) {
  const PPCContext entry = ctx;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Entry &e : g_entries) {
      if (!e.object)
        continue;
      if (!Alive(e)) {
        e = Entry{};
        continue;
      }
      if (e.has_previous) {
        WritePose(e.object, e.last);
        UpdateWorldMatrix(entry, base, e.object);
      }
    }
  }
  g_in_update.store(true, std::memory_order_release);
  __imp__eot_PhysicsWorld_Update(ctx, base);
  g_in_update.store(false, std::memory_order_release);

  const uint32_t steps = eot::mem::load<uint32_t>(kWorldTotalSteps);
  const float step = LoadF(kWorldStep);
  float alpha = step > 0.0f ? LoadF(kWorldAccumulator) / step : 1.0f;
  alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Entry &e : g_entries) {
    if (!e.object)
      continue;
    if (e.step + 1 != steps || !Alive(e)) {
      e = Entry{};
      continue;
    }
    if (!e.has_previous)
      continue;
    WritePose(e.object, Blend(e.previous, e.last, alpha));
    UpdateWorldMatrix(entry, base, e.object);
    ++g_drawn_between;
  }

  using clock = std::chrono::steady_clock;
  static clock::time_point last = clock::now();
  if (clock::now() - last >= std::chrono::seconds(30)) {
    last = clock::now();
    if (g_placements)
      EOT_INFO("[physics] last 30 s: {} placements from inside a step, {} frames drawn between two", g_placements,
               g_drawn_between);
    g_placements = g_drawn_between = 0;
  }
}
