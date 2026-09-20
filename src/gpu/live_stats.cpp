#include "gpu/live_stats.h"

#include <mutex>

namespace eot::gpu {

namespace {
std::mutex g_mutex;
LiveStats g_stats;
bool g_published = false;
}

void PublishLiveStats(const LiveStats &stats) {
  std::lock_guard lock(g_mutex);
  const u64 sequence = g_stats.sequence + 1;
  g_stats = stats;
  g_stats.sequence = sequence;
  g_published = true;
}

bool ReadLiveStats(LiveStats *out) {
  std::lock_guard lock(g_mutex);
  if (!g_published)
    return false;
  *out = g_stats;
  return true;
}

}
