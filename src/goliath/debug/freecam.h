#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include "goliath/controller/pad_remap.h"

namespace eot::debug {

void FreecamTick();

bool FreecamActive();

bool FreecamDrawTwoSided();

bool FreecamReadout(float &x, float &y, float &z, float &yaw_degrees, float &pitch_degrees,
                    double &speed);

}

namespace eot::debug {

void ScenePauseTick();

bool ScenePauseActive();

void ScenePauseStep();

int ScenePauseStepsPending();

}

namespace eot::debug {

bool OcclusionSuppressed();

}

namespace eot::debug {

uint32_t InputScriptSet(std::string_view script);

void InputScriptReplay();

void InputScriptTick(bool simulating);

bool InputScriptPad(controller::RawPad &pad);

bool InputScriptHoldsInput();

std::string InputScriptReadout();

}
