#pragma once

#include <cstdint>
#include <vector>

namespace eot::gpu {

struct DeclElem {
  uint16_t stream = 0;
  uint16_t offset = 0;
  uint32_t type = 0;
  uint8_t usage = 0;
  uint8_t usageIndex = 0;
};

bool DeclElementsFor(uint32_t pDecl, std::vector<DeclElem>& out);

}
