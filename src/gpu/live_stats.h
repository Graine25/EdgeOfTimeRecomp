#pragma once

#include <rex/types.h>

namespace eot::gpu {

struct LiveStats {
  u64 sequence = 0;
  f32 wall_ms = 0, gpu_ms = 0, capture_ms = 0, present_wait_ms = 0, draw_ms = 0, record_ms = 0;
  u32 draws = 0, resolves = 0;
  struct Row {
    char name[20] = {};
    f32 ms = 0;
    u32 draws = 0;
  };
  static constexpr u32 kMaxRows = 8;
  Row rows[kMaxRows];
  u32 row_count = 0;
};

void PublishLiveStats(const LiveStats &stats);
bool ReadLiveStats(LiveStats *out);

}
