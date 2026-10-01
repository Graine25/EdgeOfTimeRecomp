#include "goliath/title_matte.h"

#include <atomic>
#include <bit>
#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/patches/aspect_ratio.h"

REX_EXTERN(__imp__eot_GRRender3dObj_AppendSubmeshDrawRecord);

namespace {

constexpr uint32_t kFindGameObjFromCRC = 0x820BFDC0;
constexpr uint32_t kHandleResolve = 0x820F3130;
constexpr uint32_t k3dObjType = 0x102;
constexpr uint32_t kCardCrc = 0x940EBBAB;
constexpr uint32_t kTitleCrc = 0x2CF5252B;

constexpr uint32_t kRecordWorld = 92;
constexpr uint32_t kRowZ = 2;
constexpr uint32_t kRowPos = 3;

constexpr float kAuthored = 16.0f / 9.0f;
constexpr float kPivotZ = -1.42f;
constexpr float kMargin = 1.05f;
constexpr uint32_t kSearchEvery = 4;

uint32_t g_card_handle = 0;
uint32_t g_title_handle = 0;
uint32_t g_search = 0;
bool g_announced = false;
std::atomic<uint32_t> g_card{0};
std::atomic<float> g_stretch{1.0f};

bool Invalid(uint32_t handle) { return handle == 0 || handle == 0xFFFFFFFFu || handle == 0x80000001u; }

float LoadF(uint32_t at) { return std::bit_cast<float>(eot::mem::load<uint32_t>(at)); }
void StoreF(uint32_t at, float v) { eot::mem::store<uint32_t>(at, std::bit_cast<uint32_t>(v)); }

uint32_t CallAt(const PPCContext &ctx, uint8_t *base, uint32_t addr, uint32_t r3, uint32_t r4 = 0,
                uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0) {
  PPCFunc *fn = rex::runtime::ResolveIndirectFunction(addr);
  if (!fn)
    return 0;
  PPCContext call = ctx;
  call.r3.u32 = r3;
  call.r4.u32 = r4;
  call.r5.u32 = r5;
  call.r6.u32 = r6;
  call.r7.u32 = r7;
  fn(call, base);
  return call.r3.u32;
}

uint32_t Resolve(const PPCContext &ctx, uint8_t *base, uint32_t handle) {
  return Invalid(handle) ? 0 : CallAt(ctx, base, kHandleResolve, handle, 0, 0, k3dObjType, 0);
}

uint32_t Find(const PPCContext &ctx, uint8_t *base, uint32_t crc, uint32_t &handle, bool search) {
  if (uint32_t object = Resolve(ctx, base, handle))
    return object;
  handle = 0;
  if (!search)
    return 0;
  const uint32_t found = CallAt(ctx, base, kFindGameObjFromCRC, k3dObjType, crc);
  const uint32_t object = Resolve(ctx, base, found);
  if (object)
    handle = found;
  return object;
}

void Stretch(uint32_t world, float s) {
  for (uint32_t k = 0; k < 3; ++k) {
    const uint32_t axis_at = world + (kRowZ * 4 + k) * 4;
    const uint32_t pos_at = world + (kRowPos * 4 + k) * 4;
    const float axis = LoadF(axis_at);
    StoreF(pos_at, LoadF(pos_at) + axis * kPivotZ * (1.0f - s));
    StoreF(axis_at, axis * s);
  }
}

}

namespace eot::goliath {

void TitleMatteTick(const PPCContext &ctx, uint8_t *base) {
  const float aspect = eot::gpu::ConfiguredAspectRatio();
  if (!(aspect > kAuthored + 0.01f)) {
    g_card.store(0, std::memory_order_relaxed);
    return;
  }
  const float stretch = aspect / kAuthored * kMargin;
  g_stretch.store(stretch, std::memory_order_relaxed);

  const bool search = g_search++ % kSearchEvery == 0;
  uint32_t card = 0;
  if (Find(ctx, base, kTitleCrc, g_title_handle, search))
    card = Find(ctx, base, kCardCrc, g_card_handle, search);
  if (card && !g_announced)
    EOT_INFO("[title] the menu's darkening card (HUD_BlackFade) found; widened x{:.3f} for a {:.3f} display", stretch,
             aspect);
  g_announced = card != 0;
  g_card.store(card, std::memory_order_relaxed);
}

}

REX_HOOK_RAW(eot_GRRender3dObj_AppendSubmeshDrawRecord) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t record = ctx.r7.u32;
  if (object != 0 && record != 0 && object == g_card.load(std::memory_order_relaxed))
    Stretch(record + kRecordWorld, g_stretch.load(std::memory_order_relaxed));
  __imp__eot_GRRender3dObj_AppendSubmeshDrawRecord(ctx, base);
}
