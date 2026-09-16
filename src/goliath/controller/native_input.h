#pragma once

#include <cstdint>

namespace eot::controller {

bool NativeInputActive();

uint64_t LastHostInputPoll();
uint64_t LastPadInputPoll();

}
