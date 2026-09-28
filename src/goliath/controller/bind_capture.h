#pragma once

#include "goliath/controller/pad_remap.h"

namespace rex::ui {
class Window;
}

namespace eot::controller {

void AttachBindCapture(rex::ui::Window *window);

bool DebugModeActive();

bool FilterPadForCapture(RawPad &pad);

}
