#pragma once

#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>

#include "core/memory_helpers.h"
#include "core/quit_client.h"
#include "goliath/ui/menu_handles.h"

REX_EXTERN(__imp__eot_YesNoWindow_InitConfig);
REX_EXTERN(__imp__eot_YesNoWindow_Open);

namespace eot::ui {

inline constexpr uint32_t kDescCount = 0;
inline constexpr uint32_t kDescSelected = 4;
inline constexpr uint32_t kDescHandle0 = 12;
inline constexpr uint32_t kDescProp0 = 52;
inline constexpr uint32_t kDescSelectableMask = 92;
inline constexpr uint32_t kDescDisabledMask = 94;
inline constexpr uint32_t kDescMaxSlots = 10;
inline constexpr uint32_t kPropDefault = 16;

inline constexpr uint32_t kEvtType = 0;
inline constexpr uint32_t kEvtConsumed = 8;
inline constexpr uint32_t kEvtNav = 7;
inline constexpr uint32_t kEvtSelect = 9;
inline constexpr uint32_t kEvtBack = 10;
inline constexpr uint32_t kEvtCancel = 12;

inline constexpr uint32_t kYesNoResultMessage = 0x0649D236;
inline constexpr uint32_t kResultHandleOff = 0;
inline constexpr uint32_t kResultValueOff = 8;
inline constexpr uint32_t kResultYes = 1;

struct YesNoLayout {
  uint32_t config;
  uint32_t titleIndex;
  uint32_t body;
  uint32_t handle;
  uint32_t state;
};

inline uint32_t DescHandleAddr(uint32_t desc, uint32_t i) { return desc + kDescHandle0 + i * 4; }
inline uint32_t DescPropAddr(uint32_t desc, uint32_t i) { return desc + kDescProp0 + i * 4; }

inline int AppendEntry(uint32_t desc, uint32_t handle) {
  const uint32_t count = eot::mem::load<uint32_t>(desc + kDescCount);
  if (count == 0 || count >= kDescMaxSlots)
    return -1;
  eot::mem::store<uint32_t>(DescHandleAddr(desc, count), handle);
  eot::mem::store<uint32_t>(DescPropAddr(desc, count), kPropDefault);
  eot::mem::store<uint32_t>(desc + kDescCount, count + 1);
  return static_cast<int>(count);
}

inline void ConsumeEvent(uint32_t event) {
  if (event)
    eot::mem::store<uint8_t>(event + kEvtConsumed, 0);
}

inline uint32_t OpenExitConfirm(const PPCContext &ctx, uint8_t *base, uint32_t self,
                                const YesNoLayout &L) {
  const uint32_t config = self + L.config;
  PPCContext call = ctx;
  call.r3.u32 = config;
  __imp__eot_YesNoWindow_InitConfig(call, base);
  const uint32_t title_index = eot::mem::load<uint32_t>(self + L.titleIndex);
  eot::mem::store<uint32_t>(config + (title_index + 8) * 4, kHandleExitTitle);
  eot::mem::store<uint32_t>(self + L.body, kHandleExitBody);
  if (L.state)
    eot::mem::store<uint32_t>(self + L.state, 2);
  call = ctx;
  call.r3.u32 = config;
  __imp__eot_YesNoWindow_Open(call, base);
  const uint32_t handle = call.r3.u32;
  eot::mem::store<uint32_t>(self + L.handle, handle);
  return handle;
}

inline bool ExitIfConfirmed(uint32_t self, uint32_t message, uint32_t result,
                            const YesNoLayout &L) {
  if (message != kYesNoResultMessage || !self || !result)
    return false;
  const uint32_t handle = eot::mem::load<uint32_t>(result + kResultHandleOff);
  if (handle != eot::mem::load<uint32_t>(self + L.handle))
    return false;
  if (eot::mem::load<uint32_t>(result + kResultValueOff) == kResultYes)
    QuitProcessFromModule(0);
  return true;
}

}
