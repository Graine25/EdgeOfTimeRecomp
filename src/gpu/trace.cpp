#include "gpu/trace.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "core/logging.h"
#include "gpu/settings.h"

namespace eot::gpu::trace {

namespace {

struct State {
  std::mutex mutex;
  std::vector<std::string> lines;
  u32 counters[static_cast<u32>(Counter::Count)] = {};
  std::atomic<i32> frames_left{-1};
  u64 frames_seen = 0;
};

State &st() {
  static State s;
  return s;
}

void InitOnce(State &s) {
  if (s.frames_left.load(std::memory_order_relaxed) == -1)
    s.frames_left.store(Settings::TraceFrames(), std::memory_order_relaxed);
}

const char *CounterName(Counter c) {
  switch (c) {
  case Counter::DrawVertices:
    return "draw";
  case Counter::DrawIndexed:
    return "drawIdx";
  case Counter::DrawDropped:
    return "dropped";
  case Counter::Clear:
    return "clear";
  case Counter::Resolve:
    return "resolve";
  case Counter::SetRenderTarget:
    return "setRT";
  case Counter::SetDepth:
    return "setDS";
  case Counter::SetTexture:
    return "setTex";
  case Counter::SetVertexShader:
    return "setVS";
  case Counter::SetPixelShader:
    return "setPS";
  case Counter::SetStreamSource:
    return "setVB";
  case Counter::SetIndices:
    return "setIB";
  case Counter::SetViewport:
    return "setVP";
  case Counter::BeginVertices:
    return "beginVerts";
  case Counter::ShaderMiss:
    return "shaderMiss";
  default:
    return "?";
  }
}

}

bool Enabled() {
  auto &s = st();
  InitOnce(s);
  if (s.frames_left.load(std::memory_order_relaxed) <= 0)
    return false;
  return static_cast<i64>(s.frames_seen) >= Settings::TraceStartFrame();
}

void Line(std::string_view text) {
  auto &s = st();
  std::lock_guard lock(s.mutex);
  if (s.lines.size() < 20000)
    s.lines.emplace_back(text);
}

void Bump(Counter c) {
  auto &s = st();
  std::lock_guard lock(s.mutex);
  s.counters[static_cast<u32>(c)]++;
}

void EndFrame(u64 frame_index) {
  auto &s = st();
  InitOnce(s);
  std::lock_guard lock(s.mutex);
  s.frames_seen++;
  if (s.frames_left.load(std::memory_order_relaxed) > 0 &&
      static_cast<i64>(s.frames_seen) > Settings::TraceStartFrame()) {
    EOT_INFO("[d3d-trace] ---- frame {} begin ({} calls) ----", frame_index, s.lines.size());
    for (size_t i = 0; i < s.lines.size(); ++i)
      EOT_INFO("[d3d-trace] {:5} {}", i, s.lines[i]);
    EOT_INFO("[d3d-trace] ---- frame {} end ----", frame_index);
    s.frames_left.fetch_sub(1, std::memory_order_relaxed);
  }
  s.lines.clear();
  if (static_cast<i64>(s.frames_seen) <= Settings::SummaryFrames()) {
    std::string summary;
    for (u32 i = 0; i < static_cast<u32>(Counter::Count); ++i) {
      if (s.counters[i])
        summary += std::format(" {}={}", CounterName(static_cast<Counter>(i)), s.counters[i]);
    }
    EOT_INFO("[d3d-frame] {}:{}", frame_index, summary);
  }
  for (auto &c : s.counters)
    c = 0;
}

void PresentMarker(u64 frame_index) {
  if (!Settings::PresentFrameLog())
    return;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  EOT_INFO("[present] frame {} t={}", frame_index,
           std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}
