#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>

#include "core/memory_helpers.h"

REXCVAR_DEFINE_BOOL(eot_debug_mode, false, "EdgeOfTime/Config", "Developer level select menu")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__eot_GLInstanciateHUDLevelSelect);
REX_EXTERN(__imp__eot_GLInstanciateHUDDebugLevelSelection);
REX_EXTERN(__imp__eot_HUDDebugLevelSelection_Update);
REX_EXTERN(__imp__eot_GameStateManagerSM2011_ReturnToFrontscreen);

namespace {

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

bool BackPressed(const PPCContext &ctx, uint8_t *base) {
  const uint32_t api = eot::mem::load<uint32_t>(kInputApi);
  const uint32_t query = api ? eot::mem::load<uint32_t>(api + kInputQuery) : 0;
  return query && CallGuest(ctx, base, query, kActionBack, kJustPressed) != 0;
}

bool g_returning = false;

}

REX_HOOK_RAW(eot_GLInstanciateHUDLevelSelect) {
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
  __imp__eot_HUDDebugLevelSelection_Update(ctx, base);
}
