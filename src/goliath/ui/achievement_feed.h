#pragma once

#include <cstdint>

#include "core/export.h"

namespace rex::system {
struct AchievementEvent;
}

namespace eot::ui {

inline constexpr uint32_t kAchGraine25 = 0x10001;
inline constexpr uint32_t kAchSerJar03 = 0x10002;
inline constexpr uint32_t kAchMaff = 0x10003;

void RegisterPortAchievements();

void QueueAchievementToast(const rex::system::AchievementEvent &event);

}

extern "C" {
EOT_EXPORT int32_t eot_ach_toast_take(char *name, int32_t size, int32_t *gamerscore, int32_t *image_id);
}
