#pragma once

#include <cstdint>

#include "goliath/controller/pad_actions.h"

namespace eot::controller {

PadInput PhysicalFor(const PadAction &action);
bool SticksSwapped();
void PadRemapChanged();
bool PadRemapActive();

struct RawPad {
  uint16_t buttons;
  uint8_t left_trigger, right_trigger;
  int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
};
void RemapPad(RawPad &pad);

}
