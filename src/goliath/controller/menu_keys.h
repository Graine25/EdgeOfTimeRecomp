#pragma once

#include "goliath/controller/pad_remap.h"

namespace rex::ui {
class Window;
}

namespace eot::controller {

void AttachMenuKeys(rex::ui::Window *window);

bool MenuKeysActive();

void ApplyMenuKeys(RawPad &pad);

void NoteMashPrompt();

}
