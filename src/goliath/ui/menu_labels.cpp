#include <cstdint>
#include <cstring>

#include <rex/hook.h>

#include "core/memory_helpers.h"
#include "goliath/ui/menu_handles.h"

REX_EXTERN(__imp__sub_821813A8);

namespace {

struct SyntheticLabel {
  uint32_t handle;
  const char *text;
};
constexpr SyntheticLabel kSyntheticLabels[] = {
    {eot::ui::kHandleExitGame, "Exit Game"},
    {eot::ui::kHandleExitTitle, "Exit Game?"},
    {eot::ui::kHandleExitBody, "Return to desktop?"},
    {eot::ui::kHandleGraphics, "Graphics"},
    {eot::ui::kHandleVipUnlockCode, "Exit Game"},
};

struct NativeOverride {
  const char *from;
  const char *to;
  bool prefix;
};
constexpr NativeOverride kNativeOverrides[] = {
    {"Quit Game", "Exit To Menu", false},
    {"Go to HEROHQ.COM", "Return to the desktop.", true},
};

char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool GuestUtf16EqualsAscii(uint32_t dst, const char *s) {
  uint32_t i = 0;
  for (; s[i] != '\0'; ++i) {
    const uint16_t ch = eot::mem::load<uint16_t>(dst + i * 2);
    if (ch > 0x7F || AsciiLower(static_cast<char>(ch)) != AsciiLower(s[i]))
      return false;
  }
  return eot::mem::load<uint16_t>(dst + i * 2) == 0;
}

uint32_t WriteGuestUtf16(uint32_t dst, const char *text) {
  uint32_t i = 0;
  for (; text[i] != '\0'; ++i)
    eot::mem::store<uint16_t>(dst + i * 2, static_cast<uint16_t>(static_cast<unsigned char>(text[i])));
  eot::mem::store<uint16_t>(dst + i * 2, 0);
  return i;
}

}

REX_HOOK_RAW(sub_821813A8) {
  const uint32_t dst = ctx.r3.u32;
  const uint32_t handle = ctx.r4.u32;
  for (const auto &e : kSyntheticLabels) {
    if (handle != e.handle)
      continue;
    if (ctx.r9.u32)
      eot::mem::store<uint8_t>(ctx.r9.u32, 0);
    ctx.r3.u32 = dst ? WriteGuestUtf16(dst, e.text) : 0;
    return;
  }
  __imp__sub_821813A8(ctx, base);
  if (!dst)
    return;
  for (const auto &ov : kNativeOverrides) {
    if (GuestUtf16EqualsAscii(dst, ov.from)) {
      ctx.r3.u32 = WriteGuestUtf16(dst, ov.to);
      return;
    }
  }
}
