#pragma once

#include <algorithm>
#include <cmath>

#include <string>

#include <rex/types.h>

namespace eot::gpu {

struct Settings {
  static i32 TraceFrames();
  static i32 TraceStartFrame();
  static i32 SummaryFrames();
  static i32 DiagVerbosity();
  static bool Vsync();
  static bool VertexMirrors();
  static bool ConstRange();
  static bool ResolveCopy();
  static f64 RenderScale();
  static std::string Resolution();
  static i32 FpsLimit();
  static std::string PsoDir();
  static std::string PsoTag();
  static bool PsoCapture();
  static bool PsoCompiledIn();
  static bool PsoPredict();
  static bool PsoPredictAll();
  static i32 PsoPredictFallback();
  static i32 PsoGateMs();
  static i32 PsoThreads();
  static i32 PerfFrames();
  static i32 DumpEvery();
  static i32 DiagFrame();
  static i32 RenderDocFrame();
  static std::string RenderDocDll();
  static std::string RenderDocPath();
  static bool PresentGamma();
  static bool PresentFrameLog();
};

constexpr u32 kGuestRenderWidth = 1120;
constexpr u32 kGuestRenderHeight = 632;

f32 RenderScaleFactor();
u32 InternalRenderWidth();
u32 InternalRenderHeight();
inline i32 ScalePx(i32 v) { return static_cast<i32>(std::lround(v * RenderScaleFactor())); }
inline u32 ScaleDim(u32 v) {
  return std::max(1u, static_cast<u32>(std::lround(v * RenderScaleFactor())));
}

}
