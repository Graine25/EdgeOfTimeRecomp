#include "gpu/patches/present_effects.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(eot_upscale, "bicubic", "EdgeOfTime/Graphics",
                      "Filter used when the window is larger than the internal render: "
                      "bilinear; bicubic (Catmull-Rom through five bilinear taps, as sharp "
                      "as Lanczos-2 at a third of its cost); or lanczos (4x4 Lanczos-2, "
                      "sixteen taps). A window smaller than the render by a whole factor is "
                      "always box-filtered, which is how supersampling resolves.")
    .allowed({"bilinear", "bicubic", "lanczos"});

namespace eot::gpu {

void SelectPresentBlitMode(u32 src_w, u32 src_h, float dst_w, float dst_h, float extra[4]) {
  extra[0] = extra[1] = extra[2] = extra[3] = 0.0f;
  if (!src_w || !src_h || dst_w <= 0.0f || dst_h <= 0.0f)
    return;
  const float rx = static_cast<float>(src_w) / dst_w;
  const float ry = static_cast<float>(src_h) / dst_h;
  if (rx >= 1.9f && ry >= 1.9f) {
    const float n = std::round(std::min(rx, ry));
    if (std::fabs(rx - n) < 0.06f && std::fabs(ry - n) < 0.06f) {
      extra[0] = 1.0f;
      extra[1] = n;
      return;
    }
  }
  if (rx < 0.98f && ry < 0.98f) {
    const std::string filter = REXCVAR_GET(eot_upscale);
    if (filter == "lanczos")
      extra[0] = 2.0f;
    else if (filter == "bicubic")
      extra[0] = 3.0f;
  }
}

}
