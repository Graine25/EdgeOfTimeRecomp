#include "goliath/ui/achievement_feed.h"

#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/achievements.h>
#include <rex/system/kernel_state.h>

#include "core/logging.h"

REXCVAR_DEFINE_STRING(eot_ach_toast_test, "", "EdgeOfTime/Debug",
                      "Show the achievement banner once at startup with this name, to see how a "
                      "long one is cut. Empty shows nothing.");

namespace eot::ui {

namespace {

struct Pending {
  std::string name;
  uint32_t gamerscore = 0;
  uint32_t image_id = 0;
};

std::mutex g_mutex;
std::deque<Pending> g_queue;
constexpr size_t kMaxWaiting = 8;

constexpr uint32_t kShowWhileLocked = 0x8;

constexpr uint32_t kFirstPortImageId = 49;

rex::system::AchievementInfo Port(uint32_t id, const char *label, const char *description) {
  rex::system::AchievementInfo info;
  info.id = id;
  info.label = label;
  info.description = description;
  info.unachieved_description = description;
  info.gamerscore = 23;
  info.flags = kShowWhileLocked;
  info.image_id = kFirstPortImageId + (id - kAchGraine25);
  return info;
}

}

void RegisterPortAchievements() {
  const rex::system::AchievementInfo ours[] = {
      Port(kAchGraine25, "Graine25", "Open the Video settings."),
      Port(kAchSerJar03, "SerJar03", "Open the Graphics settings."),
      Port(kAchMaff, "Maff", "Open the Controls settings."),
  };
  uint32_t done = 0;
  for (const rex::system::AchievementInfo &info : ours)
    done += rex::system::RegisterAchievement(info) ? 1 : 0;
  EOT_INFO("[ach] {} of the port's own achievements registered (23 G each)", done);
  const std::string preview = REXCVAR_GET(eot_ach_toast_test);
  if (!preview.empty()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.push_back({preview, 23, kFirstPortImageId});
    EOT_INFO("[ach] eot_ach_toast_test: the banner will show \"{}\" once", preview);
  }
}

void QueueAchievementToast(const rex::system::AchievementEvent &event) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_queue.size() >= kMaxWaiting)
    return;
  g_queue.push_back({event.achievement.label, event.achievement.gamerscore, event.achievement.image_id});
}

}

int32_t eot_ach_toast_take(char *name, int32_t size, int32_t *gamerscore, int32_t *image_id) {
  std::lock_guard<std::mutex> lock(eot::ui::g_mutex);
  if (eot::ui::g_queue.empty())
    return 0;
  const eot::ui::Pending next = eot::ui::g_queue.front();
  eot::ui::g_queue.pop_front();
  if (name && size > 0) {
    std::strncpy(name, next.name.c_str(), static_cast<size_t>(size) - 1);
    name[size - 1] = 0;
  }
  if (gamerscore)
    *gamerscore = static_cast<int32_t>(next.gamerscore);
  if (image_id)
    *image_id = static_cast<int32_t>(next.image_id);
  return 1;
}
