#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gamelogic/ui/binds_client.h"
#include "gamelogic/ui/button_prompts.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_HUDSavegameSelect_Construct);  // (this r3, ...)
REX_EXTERN(__imp__eot_GLInstanciateHUDButtonHelper);
REX_EXTERN(__imp__sub_8827CBD8);

namespace {

constexpr uint32_t kPromptTable = 0x883C9BC8; // uint32 nameCRC[32], filled by 0x8827C3E8
constexpr uint32_t kPromptCount = 32;

struct Rewrite {
  uint32_t id;
  uint32_t retail;
  const char *replacement;
};

constexpr Rewrite kRewrites[] = {
    {22, 0x3A79CC4E, "REEOT_PROMPT_EXIT_GAME"},
};

void ApplyRewrites() {
  for (const Rewrite &r : kRewrites) {
    if (r.id >= kPromptCount)
      continue;
    const uint32_t slot = kPromptTable + r.id * 4;
    if (eot::mem::load<uint32_t>(slot) != r.retail)
      continue;
    const uint32_t crc = eot::ui::NameCrc(r.replacement);
    eot::mem::store<uint32_t>(slot, crc);
    EOT_INFO("[menu] button prompt {} -> {} (crc {:#010x})", r.id, r.replacement, crc);
  }
}

uint32_t g_held[kPromptCount] = {};

}

REX_HOOK_RAW(eot_HUDSavegameSelect_Construct) {
  __imp__eot_HUDSavegameSelect_Construct(ctx, base);
  ApplyRewrites();
}

REX_HOOK_RAW(sub_8827CBD8) {
  constexpr uint32_t kSetMaskMessage = 0xDCDC2E9F;
  const uint32_t message = ctx.r4.u32;
  const uint32_t args = ctx.r5.u32;
  __imp__sub_8827CBD8(ctx, base);
  if (message != kSetMaskMessage || !args)
    return;
  if (const auto note = eot::ui::binds::Api().bar_zone)
    note(static_cast<int32_t>(eot::mem::load<uint32_t>(args)));
}

REX_HOOK_RAW(eot_GLInstanciateHUDButtonHelper) {
  __imp__eot_GLInstanciateHUDButtonHelper(ctx, base);
  if (const auto note = eot::ui::binds::Api().bar_object)
    note(static_cast<int32_t>(ctx.r3.u32));
}

namespace eot::ui {

void OverridePrompt(uint32_t id, const char *name) {
  if (id >= kPromptCount)
    return;
  const uint32_t slot = kPromptTable + id * 4;
  if (!g_held[id])
    g_held[id] = eot::mem::load<uint32_t>(slot);
  eot::mem::store<uint32_t>(slot, eot::ui::NameCrc(name));
}

void RestorePrompt(uint32_t id) {
  if (id >= kPromptCount || !g_held[id])
    return;
  eot::mem::store<uint32_t>(kPromptTable + id * 4, g_held[id]);
  g_held[id] = 0;
}

void SendPromptMask(const PPCContext &ctx, uint8_t *base, uint32_t zone, uint32_t mask, uint32_t scratch) {
  constexpr uint32_t kApiLogicPtr = 0x883CA22C;
  constexpr uint32_t kHudsDataPtr = 0x883CA288;
  constexpr uint32_t kHelperOffset = 0xA4;
  constexpr uint32_t kSetMaskMessage = 0xDCDC2E9F;
  const uint32_t api = eot::mem::load<uint32_t>(kApiLogicPtr);
  const uint32_t huds = eot::mem::load<uint32_t>(kHudsDataPtr);
  if (!api || !huds || !scratch)
    return;
  const uint32_t helper = eot::mem::load<uint32_t>(huds + kHelperOffset);
  eot::mem::store<uint32_t>(scratch, zone);
  eot::mem::store<uint32_t>(scratch + 4, mask);
  hud::CallAt(ctx, base, eot::mem::load<uint32_t>(api), helper, 0, kSetMaskMessage, scratch);
}

}
