#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/memory_helpers.h"
#include "goliath/debug/freecam.h"
#include "goliath/debug/scene_pause.h"

REX_EXTERN(__imp__eot_GLAPIEngine_SetTimeScale);
REX_EXTERN(__imp__eot_HUDWindow_DrawTree);
REX_EXTERN(__imp__eot_Engine_UpdateInput);

REXCVAR_DEFINE_BOOL(eot_debug_pause, false, "EdgeOfTime/Debug",
                    "Freeze the scene: the engine keeps rendering but is given no time, so "
                    "nothing moves, animates, thinks or collides. Nothing in the game is told "
                    "it was paused.");

REXCVAR_DEFINE_BOOL(eot_debug_pause_hide_hud, true, "EdgeOfTime/Debug",
                    "While the scene is frozen, stop the HUD window tree from drawing, which "
                    "takes the HUD, its text and any subtitle with it. No HUD state is changed.");

namespace {

constexpr uint32_t kTimeScale = 0x824E56D4;

constexpr uint32_t kInputMask = 0x824C7A4C;

uint32_t g_saved_input_mask = 0;
bool g_input_held = false;

constexpr float kFrozenScale = 1.0e-6f;

float g_game_scale = 1.0f;

std::atomic<bool> g_frozen{false};

bool Sane(float scale) { return scale > 0.0f && std::isfinite(scale); }

float LoadScale() { return std::bit_cast<float>(eot::mem::load<uint32_t>(kTimeScale)); }

void StoreScale(float scale) {
  eot::mem::store<uint32_t>(kTimeScale, std::bit_cast<uint32_t>(scale));
}

bool HudHidden() {
  return g_frozen.load(std::memory_order_relaxed) && REXCVAR_GET(eot_debug_pause_hide_hud);
}

bool InputBlocked() {
  return g_frozen.load(std::memory_order_relaxed) || eot::debug::FreecamActive();
}

}

namespace eot::debug {

bool ScenePauseActive() { return g_frozen.load(std::memory_order_relaxed); }

void ScenePauseTick() {
  const bool want = REXCVAR_GET(eot_debug_pause);

  if (want) {
    g_frozen.store(true, std::memory_order_relaxed);
    StoreScale(kFrozenScale);
    return;
  }

  if (g_frozen.exchange(false, std::memory_order_relaxed)) {
    StoreScale(g_game_scale);
    return;
  }

  const float live = LoadScale();
  if (Sane(live))
    g_game_scale = live;
}

}

REX_HOOK_RAW(eot_GLAPIEngine_SetTimeScale) {
  const double requested = ctx.f1.f64;
  if (requested > 0.0 && std::isfinite(requested))
    g_game_scale = static_cast<float>(requested);

  if (g_frozen.load(std::memory_order_relaxed))
    return;
  __imp__eot_GLAPIEngine_SetTimeScale(ctx, base);
}

REX_HOOK_RAW(eot_HUDWindow_DrawTree) {
  if (HudHidden()) {
    ctx.r3.u32 = 0;
    return;
  }
  __imp__eot_HUDWindow_DrawTree(ctx, base);
}

REX_HOOK_RAW(eot_Engine_UpdateInput) {
  __imp__eot_Engine_UpdateInput(ctx, base);

  if (InputBlocked()) {
    g_saved_input_mask = eot::mem::load<uint32_t>(kInputMask);
    g_input_held = true;
    eot::mem::store<uint32_t>(kInputMask, 0u);
    return;
  }

  if (g_input_held) {
    g_input_held = false;
    eot::mem::store<uint32_t>(kInputMask, g_saved_input_mask);
  }
}
