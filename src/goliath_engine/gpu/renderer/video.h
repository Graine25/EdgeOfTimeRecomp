#pragma once

#include <cstdint>

struct Video {
  static inline uint32_t s_viewportWidth = 1280;
  static inline uint32_t s_viewportHeight = 720;

  static bool Init(void* nativeWindowHandle, uint32_t width, uint32_t height);

  static bool IsInitialized();

  static void Present();

  static void SetClearColor(float r, float g, float b, float a);

  static void WaitForGPU();

  static void Shutdown();
};
