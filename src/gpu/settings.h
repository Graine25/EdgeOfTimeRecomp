#pragma once

#include <string>

#include <rex/types.h>

namespace eot::gpu {

struct Settings {
  static i32 TraceFrames();
  static i32 TraceStartFrame();
  static i32 SummaryFrames();
  static i32 DiagVerbosity();
  static bool Vsync();
  static i32 PerfFrames();
  static i32 DumpEvery();
  static i32 DiagFrame();
  static i32 RenderDocFrame();
  static std::string RenderDocDll();
  static std::string RenderDocPath();
  static bool PresentGamma();
  static bool PresentFrameLog();
};

}
