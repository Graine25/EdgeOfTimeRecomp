#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gamelogic/ui/hud_api.h"

REXCVAR_DEFINE_BOOL(eot_sd_suits, false, "EdgeOfTime/Config",
                    "Unlock the eight Shattered Dimensions bonus suits (2099 Flipside, Iron, Armor, "
                    "Cosmic; Amazing Bag-Man, Cosmic, Scarlet, Secret War) as if a Shattered "
                    "Dimensions save had been found. Off leaves whatever the save file says.");

REXCVAR_DEFINE_STRING(eot_sd_code, "shatmypants", "EdgeOfTime/Config",
                      "A VIP unlock code the game's code page accepts for the Shattered Dimensions "
                      "bonus suits, compared without regard to case. Empty disables it.");

REX_EXTERN(__imp__eot_HUDStartScreen_EnterSDCheck); // (this r3)
REX_EXTERN(__imp__eot_SaveGame_ApplyLoadedData);    // (this r3, result r4)
REX_EXTERN(__imp__eot_HUDDLCCode_Validate);
REX_EXTERN(__imp__eot_TextPopup_InitConfig);        // (config r3, input block r4)
REX_EXTERN(__imp__eot_TextPopup_SetBody);           // (config r3, string handle r4)
REX_EXTERN(__imp__eot_YesNoWindow_Open);            // (config r3) -> pop-up id

namespace {

constexpr uint32_t kSdSaveSeen = 0x883DDC48;
constexpr uint32_t kBonusFlags = 0x883DD42C;
constexpr uint16_t kBonusSdWelcome = 0x100;

constexpr uint32_t kPageTyped = 220;
constexpr uint32_t kPageConfig = 1248;
constexpr uint32_t kPagePriority = 1252;
constexpr uint32_t kPageFlags = 1276;
constexpr uint32_t kPageTitleIndex = 1324;
constexpr uint32_t kPagePopupId = 1352;
constexpr uint32_t kPageAlreadyHandle = 1360;
constexpr uint32_t kPageResult = 1364;
constexpr uint32_t kConfigTitles = 32;
constexpr uint32_t kVipTitleNameCrc = 0xD67C58C4;
constexpr uint32_t kSuitUnlockedNameCrc = 0x97C2F465;

void ForceFlag(const char *where) {
  if (!REXCVAR_GET(eot_sd_suits) || eot::mem::load<uint8_t>(kSdSaveSeen) != 0)
    return;
  eot::mem::store<uint8_t>(kSdSaveSeen, 1);
  EOT_INFO("[sd] Shattered Dimensions bonus suits: flag set {}", where);
}

std::string TypedCode(uint32_t self) {
  std::string out;
  for (uint32_t i = 0; i < 128; ++i) {
    const uint16_t c = eot::mem::load<uint16_t>(self + kPageTyped + i * 2);
    if (c == 0)
      break;
    if (c >= 0x80)
      return std::string();
    out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
  }
  return out;
}

std::string PortCode() {
  std::string code = REXCVAR_GET(eot_sd_code);
  for (char &c : code)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c + 32);
  return code;
}

uint32_t CallGuest(const PPCContext &ctx, uint8_t *base, PPCFunc *fn, uint32_t r3, uint32_t r4 = 0) {
  PPCContext call = ctx;
  call.r3.u32 = r3;
  call.r4.u32 = r4;
  fn(call, base);
  return call.r3.u32;
}

void AnswerWithPopup(const PPCContext &ctx, uint8_t *base, uint32_t self, uint32_t body) {
  const uint32_t config = self + kPageConfig;
  CallGuest(ctx, base, __imp__eot_TextPopup_InitConfig, config, 1);
  const uint32_t title = eot::ui::hud::FindString(ctx, base, kVipTitleNameCrc);
  const uint32_t index = eot::mem::load<uint32_t>(self + kPageTitleIndex);
  if (title && index < 6)
    eot::mem::store<uint32_t>(config + kConfigTitles + index * 4, title);
  eot::mem::store<uint8_t>(self + kPageFlags, eot::mem::load<uint8_t>(self + kPageFlags) | 4);
  eot::mem::store<uint32_t>(self + kPagePriority, 0);
  CallGuest(ctx, base, __imp__eot_TextPopup_SetBody, config, body);
  const uint32_t id = CallGuest(ctx, base, __imp__eot_YesNoWindow_Open, config);
  eot::mem::store<uint32_t>(self + kPagePopupId, id);
}

}

REX_HOOK_RAW(eot_HUDStartScreen_EnterSDCheck) {
  ForceFlag("before the start screen's check");
  __imp__eot_HUDStartScreen_EnterSDCheck(ctx, base);
}

REX_HOOK_RAW(eot_SaveGame_ApplyLoadedData) {
  __imp__eot_SaveGame_ApplyLoadedData(ctx, base);
  ForceFlag("after the save file was read");
}

REX_HOOK_RAW(eot_HUDDLCCode_Validate) {
  const uint32_t self = ctx.r3.u32;
  const std::string code = PortCode();
  if (!self || code.empty() || TypedCode(self) != code) {
    __imp__eot_HUDDLCCode_Validate(ctx, base);
    return;
  }
  if (eot::mem::load<uint8_t>(kSdSaveSeen) != 0) {
    EOT_INFO("[sd] VIP code entered again; answering 'already entered'");
    AnswerWithPopup(ctx, base, self, eot::mem::load<uint32_t>(self + kPageAlreadyHandle));
    return;
  }
  eot::mem::store<uint8_t>(kSdSaveSeen, 1);
  eot::mem::store<uint16_t>(kBonusFlags, eot::mem::load<uint16_t>(kBonusFlags) | kBonusSdWelcome);
  eot::mem::store<uint8_t>(self + kPageResult, eot::mem::load<uint8_t>(self + kPageResult) | 0x40);
  const uint32_t unlocked = eot::ui::hud::FindString(ctx, base, kSuitUnlockedNameCrc);
  if (!unlocked)
    EOT_WARN("[sd] the game's 'new Alternate Suit' string is not loaded; the answer has no body");
  AnswerWithPopup(ctx, base, self, unlocked);
  EOT_INFO("[sd] VIP code accepted: Shattered Dimensions bonus suits unlocked");
}
