#pragma once

#include <cstdint>

namespace eot::ui {

inline constexpr uint32_t kReeotPackageId = 0x7EE;
inline constexpr const char *kReeotPackageName = "L:/ReeotUI.pak";
inline constexpr uint32_t kReeotPackageDependency = 2;
inline constexpr uint32_t kReeotHandleBase = kReeotPackageId << 20;

inline constexpr uint32_t kHandleExitGame = kReeotHandleBase | 0;
inline constexpr uint32_t kHandleExitTitle = kReeotHandleBase | 1;
inline constexpr uint32_t kHandleExitBody = kReeotHandleBase | 2;
inline constexpr uint32_t kHandleGraphics = kReeotHandleBase | 3;
inline constexpr uint32_t kHandleExitToMenu = kReeotHandleBase | 4;
inline constexpr uint32_t kHandleExitTicker = kReeotHandleBase | 5;

}
