#pragma once

#include <cstdint>

namespace eot::platform {

struct DisplaySize {
  uint32_t width = 0;
  uint32_t height = 0;
};

DisplaySize DisplayFor(void *native_window);

uint32_t AutoRenderHeight(const DisplaySize &display);

const char *AutoQualityPreset(const DisplaySize &display);

}
