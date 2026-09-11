#include "platform/display.h"

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

const char *AutoQualityPreset(const DisplaySize &display) {
  const uint32_t height = display.height ? display.height : 1080;
  return height >= 1080 ? "medium" : "low";
}

}
