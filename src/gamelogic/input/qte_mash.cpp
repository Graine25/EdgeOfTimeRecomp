#include <cstdint>

#include <rex/hook.h>

#include "core/memory_helpers.h"
#include "gamelogic/ui/binds_client.h"

REX_EXTERN(__imp__eot_ButtonMash_Update); // (helper r3, dt f1): a frame of a mash

namespace {

constexpr uint32_t kSignedInUser = 0x883CA2C0;
constexpr uint32_t kLogicInputs = 80;
constexpr uint32_t kRecordSize = 48;
constexpr uint32_t kMaxSources = 4;

constexpr uint16_t kHardwareButton[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200};

uint16_t ButtonsFor(uint32_t input) {
  if (input >= kLogicInputs)
    return 0;
  const uint32_t users = eot::mem::load<uint32_t>(kSignedInUser);
  const uint32_t table = users ? eot::mem::load<uint32_t>(users + 12) : 0;
  if (!table)
    return 0;
  const uint32_t record = table + kRecordSize * input;
  const uint32_t count = eot::mem::load<uint8_t>(record + 47);
  uint16_t buttons = 0;
  for (uint32_t i = 0; i < count && i < kMaxSources; ++i) {
    const uint32_t source = eot::mem::load<uint32_t>(record + 8 * i);
    if (!source || eot::mem::load<uint32_t>(source) != 1)
      continue;
    const uint32_t hardware = eot::mem::load<uint32_t>(source + 4);
    if (hardware < sizeof(kHardwareButton) / sizeof(kHardwareButton[0]))
      buttons |= kHardwareButton[hardware];
  }
  return buttons;
}

}

REX_HOOK_RAW(eot_ButtonMash_Update) {
  const uint32_t helper = ctx.r3.u32;
  __imp__eot_ButtonMash_Update(ctx, base);
  if (eot::mem::load<uint8_t>(helper + 52) & 0x80)
    return;
  const uint16_t buttons = ButtonsFor(eot::mem::load<uint32_t>(helper + 48));
  if (!buttons)
    return;
  if (const auto note = eot::ui::binds::Api().mash_prompt)
    note(buttons);
}
