#include <atomic>
#include <bit>
#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/ui/aspect_policy.h"

REX_EXTERN(__imp__eot_HUDText_BuildLayout);
REX_EXTERN(__imp__eot_HUDText_BuildDrawPacket);

namespace {

constexpr uint32_t kWndCrc = 292;
constexpr uint32_t kTextAttrWindow = 4;

constexpr uint32_t kLayoutScaleX = 56;
constexpr uint32_t kPacketScaleX = 60;
constexpr uint32_t kPacketShadowX = 76;

float ReadGuestF32(uint32_t va) { return std::bit_cast<float>(eot::mem::load<uint32_t>(va)); }
void ScaleGuestF32(uint32_t va, float by) {
  eot::mem::store<uint32_t>(va, std::bit_cast<uint32_t>(ReadGuestF32(va) * by));
}

std::atomic<uint32_t> g_layout_scaled{0};
std::atomic<uint32_t> g_packet_scaled{0};

uint32_t OwnerCrc(uint32_t text_attr) {
  if (!text_attr)
    return 0;
  const uint32_t wnd = eot::mem::load<uint32_t>(text_attr + kTextAttrWindow);
  return wnd ? eot::mem::load<uint32_t>(wnd + kWndCrc) : 0;
}

void ScaleLayout(uint32_t text_attr, uint32_t layout) {
  const float scale = eot::goliath::TextScaleFactor();
  if (!text_attr || !layout || scale == 0.0f)
    return;

  const float before = ReadGuestF32(layout + kLayoutScaleX);
  ScaleGuestF32(layout + kLayoutScaleX, scale);

  const uint32_t n = g_layout_scaled.fetch_add(1, std::memory_order_relaxed) + 1;
  if (eot::goliath::UiAspectLogEnabled() && n <= 24)
    EOT_INFO("[ui] text layout {:#010x} scaleX {:.4f}->{:.4f} (x{:.4f})", OwnerCrc(text_attr),
             before, ReadGuestF32(layout + kLayoutScaleX), scale);
}

void ScalePacket(uint32_t text_attr, uint32_t packet, bool built) {
  const float scale = eot::goliath::TextScaleFactor();
  if (!packet || !built || scale == 0.0f)
    return;

  const float before = ReadGuestF32(packet + kPacketScaleX);
  ScaleGuestF32(packet + kPacketScaleX, scale);
  ScaleGuestF32(packet + kPacketShadowX, scale);

  const uint32_t n = g_packet_scaled.fetch_add(1, std::memory_order_relaxed) + 1;
  if (eot::goliath::UiAspectLogEnabled() && n <= 24)
    EOT_INFO("[ui] text packet {:#010x} quadScaleX {:.4f}->{:.4f} (x{:.4f})", OwnerCrc(text_attr),
             before, ReadGuestF32(packet + kPacketScaleX), scale);
}

}

REX_HOOK_RAW(eot_HUDText_BuildLayout) {
  const uint32_t text_attr = ctx.r3.u32;
  const uint32_t layout = ctx.r4.u32;
  __imp__eot_HUDText_BuildLayout(ctx, base);
  if (ctx.r3.u32 != 0)
    ScaleLayout(text_attr, layout);
}

REX_HOOK_RAW(eot_HUDText_BuildDrawPacket) {
  const uint32_t text_attr = ctx.r3.u32;
  const uint32_t packet = ctx.r5.u32;
  __imp__eot_HUDText_BuildDrawPacket(ctx, base);
  ScalePacket(text_attr, packet, ctx.r3.u32 != 0);
}
