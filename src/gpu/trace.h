#pragma once

#include <format>
#include <string_view>

#include <rex/types.h>

namespace eot::gpu::trace {

enum class Counter : u32 {
  DrawVertices,
  DrawIndexed,
  DrawDropped,
  Clear,
  Resolve,
  SetRenderTarget,
  SetDepth,
  SetTexture,
  SetVertexShader,
  SetPixelShader,
  SetStreamSource,
  SetIndices,
  SetViewport,
  BeginVertices,
  ShaderMiss,
  Count
};

bool Enabled();
void Line(std::string_view text);
void Bump(Counter c);
void EndFrame(u64 frame_index);
void PresentMarker(u64 frame_index);

}

#define EOT_TRACE_CALL(...)                                                                    \
  do {                                                                                         \
    if (::eot::gpu::trace::Enabled())                                                          \
      ::eot::gpu::trace::Line(std::format(__VA_ARGS__));                                       \
  } while (0)
