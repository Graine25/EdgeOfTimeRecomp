#include <cstdint>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_GLInstanciateGenericSectionControl); // (params r3, out r4, out r5) -> control r3

namespace {
using eot::mem::load;
using eot::mem::store;

constexpr uint32_t kControlSectionId = 40;
constexpr uint32_t kControlParams = 44;
constexpr uint32_t kParamsLightManager = 20;
constexpr uint32_t kNone = 0xFFFFFFFFu;

struct InheritedLights {
  uint32_t section;
  uint32_t light_manager;
};

constexpr uint32_t kLightManagerSession5 = 0x338E6638;
constexpr InheritedLights kInherited[] = {
    {32, kLightManagerSession5},
    {33, kLightManagerSession5},
    {34, kLightManagerSession5},
    {35, kLightManagerSession5},
    {42, kLightManagerSession5},
};
}

REX_HOOK_RAW(eot_GLInstanciateGenericSectionControl) {
  __imp__eot_GLInstanciateGenericSectionControl(ctx, base);
  const uint32_t control = ctx.r3.u32;
  if (!control)
    return;
  const uint32_t params = load<uint32_t>(control + kControlParams);
  if (!params || load<uint32_t>(params + kParamsLightManager) != kNone)
    return;
  const uint32_t section = load<uint32_t>(control + kControlSectionId);
  for (const auto &entry : kInherited) {
    if (entry.section != section)
      continue;
    store<uint32_t>(params + kParamsLightManager, entry.light_manager);
    EOT_INFO("[lights] section {} takes LightManager {:#010x} (the one the sections before it applied)", section,
             entry.light_manager);
    return;
  }
}
