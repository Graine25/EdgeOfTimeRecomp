#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"

REX_EXTERN(__imp__sub_880C0180); // CombatManager::UpdatePlayingMusic(this r3)

REX_HOOK_RAW(sub_880C0180) {
  if (!rex::cvar::Query<bool>("eot_battle_theme"))
    return;
  __imp__sub_880C0180(ctx, base);
}
