#pragma once

#include <cstdint>

namespace eot::ui {

inline constexpr uint32_t kHandleExitGame = 0x00280FE0;
inline constexpr uint32_t kHandleExitTitle = 0x00280E10;
inline constexpr uint32_t kHandleExitBody = 0x00280E11;
inline constexpr uint32_t kHandleGraphics = 0x00280F00;
inline constexpr uint32_t kHandleVipUnlockCode = 0x00280293;

inline constexpr const char *kGraphicsMenuCvar = "eot_graphics_menu";

}
