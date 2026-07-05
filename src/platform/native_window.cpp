/**
 * @file    platform/native_window.cpp
 * @brief   Build a plume::RenderWindow swap-chain target from the app window.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/native_window.h"

#include <rex/ui/window.h>

#include "core/logging.h"

namespace eot::platform {

bool GetNativeRenderWindow(rex::ui::Window *window, plume::RenderWindow &out) {
  out = static_cast<plume::RenderWindow>(window->GetNativeWindowHandle());
  if (!out) {
    EOT_ERROR("Window has no native HWND yet");
    return false;
  }
  return true;
}

}
