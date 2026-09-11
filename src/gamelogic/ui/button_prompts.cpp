#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_HUDSavegameSelect_Construct); // (this r3, ...)

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

}

REX_HOOK_RAW(eot_HUDSavegameSelect_Construct) {
  __imp__eot_HUDSavegameSelect_Construct(ctx, base);
  ApplyRewrites();
}
