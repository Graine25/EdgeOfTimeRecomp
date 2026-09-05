#include "goliath/ui/aspect_policy.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/patches/aspect_ratio.h"

REXCVAR_DEFINE_STRING(eot_ui_aspect, "lock", "eot",
                      "HUD layout when the display is wider than 16:9: lock holds it in a centred "
                      "16:9 region (backgrounds still cover the screen), stretch is the retail "
                      "full-width behaviour.")
    .allowed({"lock", "stretch"});

REXCVAR_DEFINE_BOOL(eot_ui_aspect_log, false, "eot",
                    "Trace the HUD aspect remap per window: CRC, policy, and the rect before and "
                    "after (the first 24 remaps).");

REX_EXTERN(__imp__sub_82182200);

namespace {

constexpr uint32_t kHudCanvasWidth = 0x824AB948;
constexpr uint32_t kHudCanvasHeight = 0x824AB94C;

constexpr uint32_t kWndFlags = 52;
constexpr uint32_t kWndParent = 80;
constexpr uint32_t kWndComputedX = 92;
constexpr uint32_t kWndComputedW = 100;
constexpr uint32_t kWndClippedX = 108;
constexpr uint32_t kWndClippedW = 116;
constexpr uint32_t kWndCrc = 292;

constexpr uint32_t kFlagScreenSpace = 0x2;

constexpr float kUiAspect = 16.0f / 9.0f;
constexpr float kAspectEpsilon = 0.01f;

enum class Policy { Lock, Stretch, LockText };

const std::unordered_map<uint32_t, Policy> &PolicyTable() {
  static const std::unordered_map<uint32_t, Policy> table = {
      {0x9086C32F, Policy::LockText},
      {0x2F9BA287, Policy::LockText},
      {0x3BE27DD9, Policy::Stretch},
      {0x237EE83C, Policy::Stretch},
      {0x8B1B2876, Policy::Stretch},
      {0x103255B9, Policy::Stretch},
      {0xC07F069D, Policy::Stretch},
      {0x24299048, Policy::Stretch},
      {0xA49D61F9, Policy::Stretch},
      {0x16B95348, Policy::Stretch},
  };
  return table;
}

Policy PolicyForCrc(uint32_t crc) {
  const auto &table = PolicyTable();
  const auto it = table.find(crc);
  return it == table.end() ? Policy::Lock : it->second;
}

const char *PolicyName(Policy p) {
  switch (p) {
  case Policy::Lock:
    return "lock";
  case Policy::Stretch:
    return "stretch";
  case Policy::LockText:
    return "lock_text";
  }
  return "?";
}

bool LockRequested() { return std::string(REXCVAR_GET(eot_ui_aspect)) == "lock"; }

float ReadGuestF32(uint32_t va) { return std::bit_cast<float>(eot::mem::load<uint32_t>(va)); }
void WriteGuestF32(uint32_t va, float v) {
  eot::mem::store<uint32_t>(va, std::bit_cast<uint32_t>(v));
}

std::atomic<uint32_t> g_remapped{0};
std::atomic<bool> g_canvas_warned{false};

void ApplyAspectPolicy(uint32_t wnd) {
  if (!wnd || !LockRequested())
    return;

  const float display_aspect = eot::gpu::ConfiguredAspectRatio();
  if (display_aspect <= kUiAspect + kAspectEpsilon)
    return;

  const uint32_t flags = eot::mem::load<uint32_t>(wnd + kWndFlags);
  const uint32_t parent = eot::mem::load<uint32_t>(wnd + kWndParent);
  const bool self_laid_out = parent == 0 || (flags & kFlagScreenSpace) != 0;

  const uint32_t crc = eot::mem::load<uint32_t>(wnd + kWndCrc);
  const Policy policy = PolicyForCrc(crc);
  if (self_laid_out == (policy == Policy::Stretch))
    return;
  const bool inverse = !self_laid_out;

  const float canvas_w = static_cast<float>(eot::mem::load<uint32_t>(kHudCanvasWidth));
  const float canvas_h = static_cast<float>(eot::mem::load<uint32_t>(kHudCanvasHeight));
  if (canvas_w <= 0.0f || canvas_h <= 0.0f) {
    if (!g_canvas_warned.exchange(true))
      EOT_WARN("[ui] HUD canvas reads {}x{}: the layout is not where kHudCanvasWidth/Height say, "
               "so no window can be remapped",
               canvas_w, canvas_h);
    return;
  }

  const float scale = kUiAspect / display_aspect;
  const float offset = canvas_w * (1.0f - scale) * 0.5f;

  const bool trace =
      eot::goliath::UiAspectLogEnabled() && g_remapped.load(std::memory_order_relaxed) < 24;
  const float before_x = ReadGuestF32(wnd + kWndComputedX);
  const float before_w = ReadGuestF32(wnd + kWndComputedW);

  const auto remap = [&](uint32_t x_field, uint32_t w_field) {
    const float x = ReadGuestF32(wnd + x_field), w = ReadGuestF32(wnd + w_field);
    if (inverse) {
      WriteGuestF32(wnd + x_field, (x - offset) / scale);
      WriteGuestF32(wnd + w_field, w / scale);
    } else {
      WriteGuestF32(wnd + x_field, x * scale + offset);
      WriteGuestF32(wnd + w_field, w * scale);
    }
  };
  remap(kWndComputedX, kWndComputedW);
  remap(kWndClippedX, kWndClippedW);
  g_remapped.fetch_add(1, std::memory_order_relaxed);

  if (trace)
    EOT_INFO("[ui] window {:#010x} {} {} canvas {}x{} ar {:.4f} scale {:.4f}: x {:.1f}->{:.1f} "
             "w {:.1f}->{:.1f}",
             crc, PolicyName(policy), inverse ? "inverse" : "forward", canvas_w, canvas_h,
             display_aspect, scale, before_x, ReadGuestF32(wnd + kWndComputedX), before_w,
             ReadGuestF32(wnd + kWndComputedW));
}

}

namespace eot::goliath {

bool UiAspectLogEnabled() { return REXCVAR_GET(eot_ui_aspect_log); }

bool UiAspectLockActive() {
  return LockRequested() && eot::gpu::ConfiguredAspectRatio() > kUiAspect + kAspectEpsilon;
}

float TextScaleFactor() {
  if (!UiAspectLockActive())
    return 0.0f;
  return kUiAspect / eot::gpu::ConfiguredAspectRatio();
}

}

REX_HOOK_RAW(sub_82182200) {
  const uint32_t window = ctx.r3.u32;
  __imp__sub_82182200(ctx, base);
  ApplyAspectPolicy(window);
}
