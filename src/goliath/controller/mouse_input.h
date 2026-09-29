#pragma once

#include "core/export.h"
#include "goliath/controller/pad_remap.h"

namespace rex::ui {
class Window;
}

namespace eot::controller {

void AttachMouseInput(rex::ui::Window *window);

void MouseCursorTick(bool overlay_wants_pointer);

void NoteMenuBarShown();

void MouseGrabForDebug(bool on);

void MouseTakeForDebug(float *dx, float *dy, int *wheel);

}

extern "C" {
EOT_EXPORT void eot_mouse_take(float *dx, float *dy);
}

namespace rex::ui {
class Window;
}

namespace eot::controller {

void AttachMenuKeys(rex::ui::Window *window);

bool MenuKeysActive();

void ApplyMenuKeys(RawPad &pad);

void NoteMashPrompt();

}
