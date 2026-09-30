#pragma once

namespace eot::gpu {

float ConfiguredAspectRatio();

void ApplyAspectRatio();

bool LayoutIsWidescreen();

bool MacWide();

class CameraRatioHold {
public:
  explicit CameraRatioHold(float value);
  ~CameraRatioHold();
  CameraRatioHold(const CameraRatioHold &) = delete;
  CameraRatioHold &operator=(const CameraRatioHold &) = delete;

private:
  unsigned saved_;
};

}

namespace eot::gpu {

bool TakeMovieDrawnFlag();

bool TakeMovieResolveSkip();

}
