#pragma once

#include "core/export.h"

namespace rex::ui {
class Window;
}

namespace eot::controller {

void AttachMouseInput(rex::ui::Window *window);

void MouseCursorTick(bool overlay_wants_pointer);

void NoteMenuBarShown();

void MouseGrabForDebug(bool on);

void MouseTakeForDebug(float *dx, float *dy, int *wheel);

void MouseAddTurn(float counts);

}

extern "C" {
EOT_EXPORT void eot_mouse_take(float *dx, float *dy);
}
