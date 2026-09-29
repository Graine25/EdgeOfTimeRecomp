#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>

#include "core/export.h"
#include "core/logging.h"

#include "core/memory_helpers.h"
#include "gamelogic/ui/menu_common.h"
#include "gamelogic/ui/hud_api.h"

REXCVAR_DEFINE_BOOL(eot_debug_mode, false, "EdgeOfTime/Config", "Developer level select menu")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__eot_GLInstanciateFrontScreenControl);
REX_EXTERN(__imp__eot_FrontScreenControl_ExitActive);
REX_EXTERN(__imp__eot_GLInstanciateHUDLevelSelect);
REX_EXTERN(__imp__eot_GLInstanciateHUDDebugLevelSelection);
REX_EXTERN(__imp__eot_HUDDebugLevelSelection_Update);
REX_EXTERN(__imp__eot_GameStateManagerSM2011_ReturnToFrontscreen);

namespace {

namespace hud = eot::ui::hud;

constexpr uint32_t kInputApi = 0x883CA224;
constexpr uint32_t kInputQuery = 4;
constexpr uint32_t kActionBack = 10;
constexpr uint32_t kJustPressed = 2;
constexpr uint32_t kFrontScreenMainMenu = 2;

uint32_t CallGuest(const PPCContext &ctx, uint8_t *base, uint32_t addr, uint32_t r3, uint32_t r4) {
  auto *state = rex::system::kernel_state();
  auto *dispatcher = state ? state->function_dispatcher() : nullptr;
  PPCFunc *fn = dispatcher ? dispatcher->GetFunction(addr) : nullptr;
  if (!fn)
    return 0;
  PPCContext call = ctx;
  call.r3.u32 = r3;
  call.r4.u32 = r4;
  fn(call, base);
  return call.r3.u32;
}

bool DebugModeEnabled() { return REXCVAR_GET(eot_debug_mode); }

void AnnounceDebugMode() {
  static bool told = false;
  if (told || !DebugModeEnabled())
    return;
  told = true;
  if (auto *tell = reinterpret_cast<void (*)(int32_t)>(eot::HostEntryPoint("eot_debug_mode_active")))
    tell(1);
  EOT_INFO("[debug] debug mode: F5 flies the camera, F6 freezes the scene, F9 steps a frame, F10 replays eot_debug_script; the SDK's own overlays (F3, F4, F7) answer only in this mode");
}

bool BackPressed(const PPCContext &ctx, uint8_t *base) {
  const uint32_t api = eot::mem::load<uint32_t>(kInputApi);
  const uint32_t query = api ? eot::mem::load<uint32_t>(api + kInputQuery) : 0;
  return query && CallGuest(ctx, base, query, kActionBack, kJustPressed) != 0;
}

bool g_returning = false;

uint32_t g_front_screen = 0;

constexpr uint32_t kPageInputMask = 116;
constexpr uint32_t kNoMask = 0xFFFFFFFFu;

constexpr uint32_t kMenuBarCrc = 0x7F85723Du;

constexpr uint32_t kZoneCount = 3;
constexpr uint32_t kMaskArgs = 8;
uint32_t g_mask_args = 0;

}

REX_HOOK_RAW(eot_GLInstanciateFrontScreenControl) {
  __imp__eot_GLInstanciateFrontScreenControl(ctx, base);
  g_front_screen = ctx.r3.u32;
  AnnounceDebugMode();
}

REX_HOOK_RAW(eot_GLInstanciateHUDLevelSelect) {
  AnnounceDebugMode();
  if (!DebugModeEnabled()) {
    __imp__eot_GLInstanciateHUDLevelSelect(ctx, base);
    return;
  }
  g_returning = false;
  __imp__eot_GLInstanciateHUDDebugLevelSelection(ctx, base);
}

REX_HOOK_RAW(eot_HUDDebugLevelSelection_Update) {
  if (DebugModeEnabled() && !g_returning && BackPressed(ctx, base)) {
    g_returning = true;
    PPCContext call = ctx;
    call.r3.u32 = kFrontScreenMainMenu;
    call.r4.u32 = 0;
    __imp__eot_GameStateManagerSM2011_ReturnToFrontscreen(call, base);
    return;
  }
  const uint32_t page = ctx.r3.u32;
  const uint32_t held = eot::mem::load<uint32_t>(page + kPageInputMask);
  __imp__eot_HUDDebugLevelSelection_Update(ctx, base);
  if (held != kNoMask && eot::mem::load<uint32_t>(page + kPageInputMask) == kNoMask) {
    if (g_front_screen) {
      PPCContext exit = ctx;
      exit.r3.u32 = g_front_screen;
      __imp__eot_FrontScreenControl_ExitActive(exit, base);
    }
    const uint32_t bar = hud::Find(ctx, base, kMenuBarCrc);
    if (bar != hud::kNoWindow)
      hud::Call(ctx, base, hud::kWndRemoveFlags, bar, 1);
    if (!g_mask_args)
      g_mask_args = eot::ui::AllocGuest(ctx, base, kMaskArgs);
    if (g_mask_args)
      for (uint32_t zone = 0; zone < kZoneCount; ++zone)
        eot::ui::SendPromptMask(ctx, base, zone, 0, g_mask_args);
  }
}
