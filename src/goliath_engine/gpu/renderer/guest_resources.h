#pragma once

#include <cstdint>

namespace eot::render {

struct DeclElem {
  uint16_t stream = 0;
  uint16_t offset = 0;
  uint32_t type = 0;
  uint8_t usage = 0;
  uint8_t usageIndex = 0;
};

struct TextureFetch {
  uint32_t dword[6] = {0, 0, 0, 0, 0, 0};
};

}
