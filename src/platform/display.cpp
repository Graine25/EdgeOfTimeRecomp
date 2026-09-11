#include "platform/display.h"

#include <cmath>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace eot::platform {

DisplaySize DisplayFor(void *native_window) {
  DisplaySize size;
#if defined(_WIN32)
  HMONITOR monitor = native_window ? ::MonitorFromWindow(static_cast<HWND>(native_window), MONITOR_DEFAULTTONEAREST)
                                   : ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (monitor && ::GetMonitorInfoW(monitor, &info)) {
    size.width = static_cast<uint32_t>(info.rcMonitor.right - info.rcMonitor.left);
    size.height = static_cast<uint32_t>(info.rcMonitor.bottom - info.rcMonitor.top);
  }
#endif
  return size;
}

uint32_t AutoRenderHeight(const DisplaySize &display) {
  const uint32_t height = display.height ? display.height : 1080;
  if (height >= 1440)
    return 1440;
  if (height >= 1080)
    return 1080;
  return 720;
}

const char *AutoResolutionPreset(const DisplaySize &display) {
  switch (AutoRenderHeight(display)) {
  case 1440:
    return "1440p";
  case 1080:
    return "1080p";
  default:
    return "720p";
  }
}

const char *AutoAspectPreset(const DisplaySize &display) {
  if (!display.width || !display.height)
    return "16:9";
  const double ratio = static_cast<double>(display.width) / static_cast<double>(display.height);
  struct Preset {
    const char *name;
    double ratio;
  };
  static constexpr Preset kPresets[] = {
      {"4:3", 4.0 / 3.0}, {"16:10", 16.0 / 10.0}, {"16:9", 16.0 / 9.0}, {"21:9", 21.0 / 9.0}, {"32:9", 32.0 / 9.0},
  };
  const Preset *best = &kPresets[2];
  for (const Preset &p : kPresets)
    if (std::fabs(p.ratio - ratio) < std::fabs(best->ratio - ratio))
      best = &p;
  return best->name;
}

const char *AutoQualityPreset(const DisplaySize &display) {
  const uint32_t height = display.height ? display.height : 1080;
  return height >= 1080 ? "medium" : "low";
}

}
