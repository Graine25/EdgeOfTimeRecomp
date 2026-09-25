#include "goliath/ui/aspect_policy.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "goliath/controller/mouse_input.h"
#include "core/memory_helpers.h"
#include "gpu/patches/aspect_ratio.h"

REXCVAR_DEFINE_STRING(eot_ui_aspect, "lock", "EdgeOfTime/Config", "HUD layout on ultrawide")
    .allowed({"lock", "stretch"});

REXCVAR_DEFINE_BOOL(eot_ui_aspect_log, false, "EdgeOfTime/Debug", "Log HUD layout decisions");

REX_EXTERN(__imp__eot_HUDWindowBC_ComputePos);

namespace {

constexpr uint32_t kHudCanvasWidth = 0x824AB948;
constexpr uint32_t kHudCanvasHeight = 0x824AB94C;

constexpr uint32_t kWndFlags = 52;
constexpr uint32_t kWndParent = 80;
constexpr uint32_t kWndComputedX = 92;
constexpr uint32_t kWndComputedY = 96;
constexpr uint32_t kWndComputedW = 100;
constexpr uint32_t kWndComputedH = 104;
constexpr uint32_t kWndClippedX = 108;
constexpr uint32_t kWndClippedW = 116;
constexpr uint32_t kWndCrc = 292;
constexpr uint32_t kMenuBarSpacerCrc = 0xAB5BEFB8u;
constexpr uint32_t kMenuBarMoreLeftCrc = 0xE790340Cu;
constexpr uint32_t kMenuBarMoreRightCrc = 0x2500B415u;
constexpr uint32_t kMenuBarSelectZoneCrc = 0xFFC59AA9u;

constexpr uint32_t kFlagScreenSpace = 0x2;

float LayoutAspect() { return eot::gpu::LayoutIsWidescreen() ? 16.0f / 9.0f : 4.0f / 3.0f; }
constexpr float kAspectEpsilon = 0.01f;

enum class Policy { Lock, Stretch, LockText, HugLeft, HugRight, LockWidth, Hide };

struct Entry {
  uint32_t crc;
  Policy policy;
  const char *name;
  bool wide;
  Policy macwide;
};

constexpr Entry kWindows[] = {
#include "hud_aspect_policy.inc"
};

const std::unordered_map<uint32_t, const Entry *> &PolicyTable() {
  static const std::unordered_map<uint32_t, const Entry *> table = [] {
    std::unordered_map<uint32_t, const Entry *> map;
    map.reserve(std::size(kWindows));
    for (const Entry &entry : kWindows)
      map.emplace(entry.crc, &entry);
    return map;
  }();
  return table;
}

const Entry *LookUp(uint32_t crc) {
  const auto &table = PolicyTable();
  const auto it = table.find(crc);
  return it == table.end() ? nullptr : it->second;
}

Policy PolicyForCrc(const Entry *entry) { return entry ? entry->policy : Policy::Lock; }

const char *PolicyName(Policy p) {
  switch (p) {
  case Policy::Lock:
    return "lock";
  case Policy::Stretch:
    return "stretch";
  case Policy::LockText:
    return "lock_text";
  case Policy::HugLeft:
    return "hug_left";
  case Policy::HugRight:
    return "hug_right";
  case Policy::LockWidth:
    return "lock_width";
  case Policy::Hide:
    return "hide";
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

void Census(uint32_t crc, const Entry *entry, uint32_t parent, bool root, float x, float w,
            float canvas_w, Policy policy) {
  if (!eot::goliath::UiAspectLogEnabled())
    return;
  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  const uint64_t key = (uint64_t(crc) << 1) | (root ? 1u : 0u);
  {
    std::lock_guard lock(mutex);
    if (seen.size() >= 512 || !seen.insert(key).second)
      return;
  }
  const uint32_t parent_crc = parent ? eot::mem::load<uint32_t>(parent + kWndCrc) : 0;
  const Entry *parent_entry = parent_crc ? LookUp(parent_crc) : nullptr;
  EOT_INFO("[ui] census crc={:#010x} {} x={:.1f} w={:.1f} ({:.0f}%..{:.0f}% of canvas) {} {} "
           "parent={:#010x} {}",
           crc, root ? "root " : "child", x, w, 100.0f * x / canvas_w,
           100.0f * (x + w) / canvas_w, PolicyName(policy),
           entry && entry->name[0] ? entry->name : "?", parent_crc,
           parent_entry && parent_entry->name[0] ? parent_entry->name : (parent ? "?" : "-"));
}

struct WideRect {
  uint32_t crc = 0;
  float x = 0, y = 0, w = 0, h = 0;
};
std::mutex g_wide_rects_mutex;
std::array<WideRect, 16> g_wide_rects{};

void NoteWideWindowRect(uint32_t crc, uint32_t wnd) {
  const WideRect now{crc, ReadGuestF32(wnd + kWndComputedX), ReadGuestF32(wnd + kWndComputedY),
                     ReadGuestF32(wnd + kWndComputedW), ReadGuestF32(wnd + kWndComputedH)};
  std::lock_guard lock(g_wide_rects_mutex);
  for (WideRect &r : g_wide_rects) {
    if (r.crc == crc || r.crc == 0) {
      r = now;
      return;
    }
  }
}

bool IsWideWindowRect(float x, float y, float w, float h) {
  std::lock_guard lock(g_wide_rects_mutex);
  for (const WideRect &r : g_wide_rects) {
    if (r.crc && std::fabs(r.x - x) < 1.0f && std::fabs(r.y - y) < 1.0f && std::fabs(r.w - w) < 1.0f &&
        std::fabs(r.h - h) < 1.0f)
      return true;
  }
  return false;
}

void ApplyAspectPolicy(uint32_t wnd) {
  if (!wnd || !LockRequested())
    return;

  const float display_aspect = eot::gpu::ConfiguredAspectRatio();
  const float ui_aspect = LayoutAspect();
  if (display_aspect <= ui_aspect + kAspectEpsilon)
    return;

  const uint32_t flags = eot::mem::load<uint32_t>(wnd + kWndFlags);
  const uint32_t parent = eot::mem::load<uint32_t>(wnd + kWndParent);
  const bool self_laid_out = parent == 0 || (flags & kFlagScreenSpace) != 0;

  const uint32_t crc = eot::mem::load<uint32_t>(wnd + kWndCrc);
  const Entry *entry = LookUp(crc);
  const bool wide_on_macwide = entry && entry->wide && eot::gpu::MacWide();
  const Policy policy = wide_on_macwide ? entry->macwide : PolicyForCrc(entry);
  const auto note_wide = [&] {
    if (wide_on_macwide)
      NoteWideWindowRect(crc, wnd);
  };
  const bool hug = policy == Policy::HugLeft || policy == Policy::HugRight;
  const bool width_only = policy == Policy::LockWidth && !self_laid_out;
  const bool hide = policy == Policy::Hide && !self_laid_out;
  Census(crc, entry, parent, self_laid_out, ReadGuestF32(wnd + kWndComputedX),
         ReadGuestF32(wnd + kWndComputedW),
         static_cast<float>(eot::mem::load<uint32_t>(kHudCanvasWidth)), policy);
  if (!hug && !width_only && !hide && self_laid_out == (policy == Policy::Stretch)) {
    note_wide();
    return;
  }
  const bool inverse = !self_laid_out && !hug && !width_only && !hide;

  const float canvas_w = static_cast<float>(eot::mem::load<uint32_t>(kHudCanvasWidth));
  const float canvas_h = static_cast<float>(eot::mem::load<uint32_t>(kHudCanvasHeight));
  if (canvas_w <= 0.0f || canvas_h <= 0.0f) {
    if (!g_canvas_warned.exchange(true))
      EOT_WARN("[ui] HUD canvas reads {}x{}: the layout is not where kHudCanvasWidth/Height say, "
               "so no window can be remapped",
               canvas_w, canvas_h);
    return;
  }

  const float scale = ui_aspect / display_aspect;
  const float offset = canvas_w * (1.0f - scale) * 0.5f;

  const bool trace = eot::goliath::UiAspectLogEnabled() &&
                     (width_only || hide || g_remapped.load(std::memory_order_relaxed) < 24);
  const float before_x = ReadGuestF32(wnd + kWndComputedX);
  const float before_w = ReadGuestF32(wnd + kWndComputedW);

  const auto anchored = [&](float x) {
    const bool inherited = !self_laid_out;
    switch (policy) {
    case Policy::HugLeft:
      return inherited ? x - offset : x * scale;
    case Policy::HugRight:
      return inherited ? x + offset : canvas_w - (canvas_w - x) * scale;
    default:
      return x * scale + offset;
    }
  };
  const auto anchored_width = [&](float w) { return hug && !self_laid_out ? w : w * scale; };
  float centre = offset + canvas_w * scale * 0.5f;
  if (width_only) {
    const uint32_t grand = parent ? eot::mem::load<uint32_t>(parent + kWndParent) : 0;
    if (grand)
      centre = ReadGuestF32(grand + kWndComputedX) + ReadGuestF32(grand + kWndComputedW) * 0.5f;
  }
  const auto remap = [&](uint32_t x_field, uint32_t w_field) {
    const float x = ReadGuestF32(wnd + x_field), w = ReadGuestF32(wnd + w_field);
    if (hide) {
      WriteGuestF32(wnd + x_field, canvas_w * 8.0f);
      return;
    }
    if (width_only) {
      WriteGuestF32(wnd + x_field, centre + (x - centre) * scale);
      WriteGuestF32(wnd + w_field, w * scale);
      return;
    }
    if (inverse) {
      WriteGuestF32(wnd + x_field, (x - offset) / scale);
      WriteGuestF32(wnd + w_field, w / scale);
    } else {
      WriteGuestF32(wnd + x_field, anchored(x));
      WriteGuestF32(wnd + w_field, anchored_width(w));
    }
  };
  remap(kWndComputedX, kWndComputedW);
  remap(kWndClippedX, kWndClippedW);
  g_remapped.fetch_add(1, std::memory_order_relaxed);
  note_wide();

  if (trace)
    EOT_INFO("[ui] window {:#010x} {} {} {} canvas {}x{} ar {:.4f} scale {:.4f}: x {:.1f}->{:.1f} "
             "w {:.1f}->{:.1f}",
             crc, entry && entry->name[0] ? entry->name : "?", PolicyName(policy),
             inverse ? "inverse" : "forward", canvas_w, canvas_h,
             display_aspect, scale, before_x, ReadGuestF32(wnd + kWndComputedX), before_w,
             ReadGuestF32(wnd + kWndComputedW));
}

}

namespace eot::goliath {

bool UiAspectLogEnabled() { return REXCVAR_GET(eot_ui_aspect_log); }

bool UiAspectLockActive() {
  return LockRequested() && eot::gpu::ConfiguredAspectRatio() > LayoutAspect() + kAspectEpsilon;
}

float TextScaleFactor() {
  if (!UiAspectLockActive())
    return 0.0f;
  return LayoutAspect() / eot::gpu::ConfiguredAspectRatio();
}

}

REX_HOOK_RAW(eot_HUDWindowBC_ComputePos) {
  const uint32_t window = ctx.r3.u32;
  __imp__eot_HUDWindowBC_ComputePos(ctx, base);
  ApplyAspectPolicy(window);
  if (window) {
    const uint32_t crc = eot::mem::load<uint32_t>(window + kWndCrc);
    if (crc == kMenuBarSpacerCrc || crc == kMenuBarMoreLeftCrc || crc == kMenuBarMoreRightCrc ||
        crc == kMenuBarSelectZoneCrc)
      eot::controller::NoteMenuBarShown();
  }
}

namespace eot::goliath {

bool HudWindowLoadsWide(uint32_t crc) {
  const Entry *entry = LookUp(crc);
  return entry && entry->wide;
}

}

REX_EXTERN(__imp__eot_PAK_BuildHUDWindow);
REX_EXTERN(__imp__eot_RenderCommand_Wnd3D);  // (renderer r3, command r4, r5)

REX_HOOK_RAW(eot_PAK_BuildHUDWindow) {
  const uint32_t crc = ctx.r7.u32;
  if (crc && eot::gpu::MacWide() && eot::goliath::HudWindowLoadsWide(crc)) {
    eot::gpu::CameraRatioHold hold(1.6f);
    __imp__eot_PAK_BuildHUDWindow(ctx, base);
    if (eot::goliath::UiAspectLogEnabled())
      EOT_INFO("[ui] macwide: window {:#010x} offered a record under the wide test -> {}", crc,
               ctx.r3.u32 ? "kept" : "refused");
    return;
  }
  __imp__eot_PAK_BuildHUDWindow(ctx, base);
}

REX_HOOK_RAW(eot_RenderCommand_Wnd3D) {
  const uint32_t cmd = ctx.r4.u32;
  if (eot::gpu::MacWide() && cmd) {
    const float x = ReadGuestF32(cmd + 28), y = ReadGuestF32(cmd + 32);
    const float w = ReadGuestF32(cmd + 36), h = ReadGuestF32(cmd + 40);
    const float canvas_w = static_cast<float>(eot::mem::load<uint32_t>(kHudCanvasWidth));
    const float canvas_h = static_cast<float>(eot::mem::load<uint32_t>(kHudCanvasHeight));
    if (w > 0.0f && h > 0.0f && canvas_w > 0.0f && canvas_h > 0.0f && IsWideWindowRect(x, y, w, h)) {
      const float scale = eot::gpu::ConfiguredAspectRatio() / (canvas_w / canvas_h) * (w / h);
      const float shift = 1.0f - (9.0f / 16.0f) * scale;
      if (shift > 0.001f) {
        const float before = ReadGuestF32(cmd + 68);
        WriteGuestF32(cmd + 68, before - shift);
        static std::atomic<int> logged{0};
        if (eot::goliath::UiAspectLogEnabled() && logged.fetch_add(1) < 4)
          EOT_INFO("[ui] macwide: 3D window {:.1f},{:.1f} {:.1f}x{:.1f}: scale {:.3f}, object moved down "
                   "{:.3f} (clip) from {:.3f}",
                   x, y, w, h, scale, shift, before);
        __imp__eot_RenderCommand_Wnd3D(ctx, base);
        WriteGuestF32(cmd + 68, before);
        return;
      }
    }
  }
  __imp__eot_RenderCommand_Wnd3D(ctx, base);
}

REX_EXTERN(__imp__eot_HUDText_BuildLayout);
REX_EXTERN(__imp__eot_HUDText_BuildDrawPacket);

namespace {

constexpr uint32_t kTextAttrWindow = 4;

constexpr uint32_t kLayoutScaleX = 56;
constexpr uint32_t kPacketScaleX = 60;
constexpr uint32_t kPacketShadowX = 76;

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

REX_EXTERN(__imp__eot_GLAPIHUD_CopyWnd);
REX_EXTERN(__imp__eot_HUDMgrBC_GetWin);

namespace {

constexpr uint32_t kParentObjectOffset = 80;
constexpr uint32_t kHandleOffset = 60;

uint32_t ResolveWindow(PPCContext &ctx, uint8_t *base, uint32_t handle) {
  const uint32_t saved_r3 = ctx.r3.u32;
  ctx.r3.u32 = handle;
  __imp__eot_HUDMgrBC_GetWin(ctx, base);
  const uint32_t window = ctx.r3.u32;
  ctx.r3.u32 = saved_r3;
  return window;
}

}

REX_HOOK_RAW(eot_GLAPIHUD_CopyWnd) {
  const uint32_t source_handle = ctx.r3.u32;
  const uint32_t source = ResolveWindow(ctx, base, source_handle);
  if (source) {
    const uint32_t parent_object = eot::mem::load<uint32_t>(source + kParentObjectOffset);
    if (parent_object) {
      const uint32_t parent_handle = eot::mem::load<uint32_t>(parent_object + kHandleOffset);
      if (!ResolveWindow(ctx, base, parent_handle)) {
        EOT_WARN("[hud] CopyWnd: window {:#x} parent handle {:#x} no longer resolves; skipping "
                 "the copy (the retail path would write through a null window)",
                 source_handle, parent_handle);
        ctx.r3.s64 = -1;
        return;
      }
    }
  }
  __imp__eot_GLAPIHUD_CopyWnd(ctx, base);
}
