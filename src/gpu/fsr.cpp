#include "gpu/fsr.h"

#include "core/logging.h"
#include "gpu/d3d.h"
#include "gpu/device.h"

namespace eot::gpu {
namespace fsr {

namespace {

struct State {
  HostTexture target;
  bool failed = false;
};

State &state_() {
  static State st;
  return st;
}

}

HostTexture *EnsureTarget(VideoState &s, u32 width, u32 height) {
  State &st = state_();
  if (st.failed || !s.device || width == 0 || height == 0)
    return nullptr;
  if (st.target.valid() && st.target.width == width && st.target.height == height)
    return &st.target;
  if (st.target.valid())
    ParkHostTexture(s, st.target);
  const plume::RenderFormat format = plume::RenderFormat::B8G8R8A8_UNORM;
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      width, height, 1, format, plume::RenderTextureFlag::RENDER_TARGET);
  st.target = HostTexture{};
  st.target.format = format;
  st.target.width = width;
  st.target.height = height;
  st.target.depth = 1;
  st.target.mipLevels = 1;
  st.target.arraySize = 1;
  st.target.sampleCount = 1;
  st.target.isDepth = false;
  st.target.renderable = true;
  if (!CreateOrRecycleHostTexture(s, st.target, desc, "fsr-easu")) {
    st.failed = true;
    EOT_ERROR("[fsr] {}x{} intermediate image failed; presenting without the filter", width,
              height);
    return nullptr;
  }
  EOT_INFO("[fsr] {}x{} intermediate image", width, height);
  return &st.target;
}

void Shutdown(VideoState &s) {
  State &st = state_();
  if (st.target.valid())
    ParkHostTexture(s, st.target);
  st.target = HostTexture{};
  st.failed = false;
}

}
}
