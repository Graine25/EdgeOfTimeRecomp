#pragma once

#include <cstdint>

namespace eot::ui {

inline constexpr uint32_t kReeotPackageId = 0x7EE;
inline constexpr const char *kReeotPackageName = "L:/custom/ReeotUI.pak";
inline constexpr const char *kReeotPackageFolder = "custom";
inline constexpr uint32_t kReeotPackageDependency = 2;
inline constexpr uint32_t kReeotHandleBase = kReeotPackageId << 20;

inline constexpr uint32_t kHandleExitGame = kReeotHandleBase | 0;
inline constexpr uint32_t kHandleExitTitle = kReeotHandleBase | 1;
inline constexpr uint32_t kHandleExitBody = kReeotHandleBase | 2;
inline constexpr uint32_t kHandleGraphics = kReeotHandleBase | 3;
inline constexpr uint32_t kHandleExitToMenu = kReeotHandleBase | 4;
inline constexpr uint32_t kHandleExitTicker = kReeotHandleBase | 5;
inline constexpr uint32_t kHandleGraphicsTitle = kReeotHandleBase | 6;
inline constexpr uint32_t kHandleGraphicsBody = kReeotHandleBase | 7;
inline constexpr uint32_t kHandleLeaveTitle = kReeotHandleBase | 8;
inline constexpr uint32_t kHandleLeaveBody = kReeotHandleBase | 9;
inline constexpr uint32_t kHandleYes = kReeotHandleBase | 10;
inline constexpr uint32_t kHandleNo = kReeotHandleBase | 11;

}
