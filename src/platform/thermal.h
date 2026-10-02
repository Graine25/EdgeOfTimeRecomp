#pragma once

namespace eot::platform {

// The OS's thermal pressure: 0 nominal, 1 fair, 2 serious (the system throttles), 3 critical; -1 where
// the OS reports none.
#if defined(__APPLE__)
int ThermalState();
#else
inline int ThermalState() { return -1; }
#endif

inline const char *ThermalStateName(int state) {
  switch (state) {
  case 0:
    return "nominal";
  case 1:
    return "fair";
  case 2:
    return "serious";
  case 3:
    return "critical";
  default:
    return "unknown";
  }
}

}
