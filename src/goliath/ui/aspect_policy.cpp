#include "goliath/ui/aspect_policy.h"

#include <array>
#include <atomic>
#include <bit>
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

REXCVAR_DEFINE_STRING(eot_ui_aspect, "lock", "EdgeOfTime/Config",
                      "HUD layout when the display is wider than 16:9: lock holds it in a centred "
                      "16:9 region (backgrounds still cover the screen), stretch is the retail "
                      "full-width behaviour.")
    .allowed({"lock", "stretch"});

REXCVAR_DEFINE_BOOL(eot_ui_aspect_log, false, "EdgeOfTime/Debug",
                    "Trace the HUD aspect policy: a census line the first time each window is "
                    "laid out (CRC, name, rect, policy and its PARENT, whether or not it moves) "
                    "plus the rect before and after for the first 24 remaps.");

REX_EXTERN(__imp__eot_HUDWindowBC_ComputePos);

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
constexpr uint32_t kMenuBarSpacerCrc = 0xAB5BEFB8u;
constexpr uint32_t kMenuBarMoreLeftCrc = 0xE790340Cu;
constexpr uint32_t kMenuBarMoreRightCrc = 0x2500B415u;
constexpr uint32_t kMenuBarSelectZoneCrc = 0xFFC59AA9u;

constexpr uint32_t kFlagScreenSpace = 0x2;

constexpr float kUiAspect = 16.0f / 9.0f;
constexpr float kAspectEpsilon = 0.01f;

enum class Policy { Lock, Stretch, LockText, HugLeft, HugRight };

struct Entry {
  uint32_t crc;
  Policy policy;
  const char *name;
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
  const Entry *entry = LookUp(crc);
  const Policy policy = PolicyForCrc(entry);
  const bool hug = policy == Policy::HugLeft || policy == Policy::HugRight;
  Census(crc, entry, parent, self_laid_out, ReadGuestF32(wnd + kWndComputedX),
         ReadGuestF32(wnd + kWndComputedW),
         static_cast<float>(eot::mem::load<uint32_t>(kHudCanvasWidth)), policy);
  if (!hug && self_laid_out == (policy == Policy::Stretch))
    return;
  const bool inverse = !self_laid_out && !hug;

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
  const auto remap = [&](uint32_t x_field, uint32_t w_field) {
    const float x = ReadGuestF32(wnd + x_field), w = ReadGuestF32(wnd + w_field);
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
  return LockRequested() && eot::gpu::ConfiguredAspectRatio() > kUiAspect + kAspectEpsilon;
}

float TextScaleFactor() {
  if (!UiAspectLockActive())
    return 0.0f;
  return kUiAspect / eot::gpu::ConfiguredAspectRatio();
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
