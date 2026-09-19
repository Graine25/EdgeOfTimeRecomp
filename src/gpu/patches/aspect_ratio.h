#pragma once

namespace eot::gpu {

float ConfiguredAspectRatio();

void ApplyAspectRatio();

bool LayoutIsWidescreen();

bool MacWide();

class CameraRatioHold {
public:
  explicit CameraRatioHold(float value, unsigned screen = 0);
  ~CameraRatioHold();
  CameraRatioHold(const CameraRatioHold &) = delete;
  CameraRatioHold &operator=(const CameraRatioHold &) = delete;

private:
  unsigned address_;
  unsigned saved_;
};

}
