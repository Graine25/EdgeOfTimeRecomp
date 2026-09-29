#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "goliath/controller/pad_remap.h"

namespace eot::debug {

uint32_t InputScriptSet(std::string_view script);

void InputScriptReplay();

void InputScriptTick(bool simulating);

bool InputScriptPad(controller::RawPad &pad);

bool InputScriptHoldsInput();

std::string InputScriptReadout();

}
