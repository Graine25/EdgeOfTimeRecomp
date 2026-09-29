#pragma once

#include <cstdint>

#include <rex/hook.h>

namespace eot::loading {

void HeapCensusTick(const PPCContext &ctx, uint8_t *base);

}
