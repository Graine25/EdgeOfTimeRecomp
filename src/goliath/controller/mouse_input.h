#pragma once

namespace rex::ui {
class Window;
}

namespace eot::controller {

void AttachMouseInput(rex::ui::Window *window);

}

extern "C" {
__declspec(dllexport) void eot_mouse_take(float *dx, float *dy);
}
