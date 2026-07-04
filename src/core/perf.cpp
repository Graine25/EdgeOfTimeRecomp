/**
 * @file    core/perf.cpp
 * @brief   Per-frame telemetry: the sample record, the history ring, and
 *          background CSV capture.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "core/perf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "core/app_root.h"
#include "core/build_info.h"
#include "core/logging.h"
#include "core/settings.h"
#include "core/time_util.h"

namespace eot {

namespace {
void AppendU64(std::string &o, u64 v) { o += std::format("{}", v); }
void AppendU32(std::string &o, u32 v) { o += std::format("{}", v); }
void AppendU8(std::string &o, u8 v) { o += std::format("{}", unsigned(v)); }
void AppendF3(std::string &o, f64 v) { o += std::format("{:.3f}", v); }
void AppendF2(std::string &o, f64 v) { o += std::format("{:.2f}", v); }

std::mutex g_mutex;
std::vector<PerfSample> g_ring;
u64 g_written = 0;

constexpr u32 kDefaultCapacity = 2400;

f64 PercentileSorted(const std::vector<f64> &sorted, f64 q) {
  if (sorted.empty())
    return 0.0;
  const f64 pos = q * f64(sorted.size() - 1);
  const size_t lo = size_t(pos);
  const size_t hi = std::min(lo + 1, sorted.size() - 1);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - f64(lo));
}

std::thread g_thread;
std::mutex g_lifecycle;
std::mutex g_csv_mutex;
std::condition_variable g_cv;
bool g_stopping = false;
std::atomic<bool> g_active{false};
std::atomic<u64> g_rows{0};
std::atomic<u64> g_dropped{0};

constexpr auto kDrainInterval = std::chrono::milliseconds(250);
constexpr u32 kBatch = 512;

std::string FileTimestamp() {
  const auto tt =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local_tm = eot::LocalTime(tt);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &local_tm);
  return buf;
}

void WriteMeta(const std::filesystem::path &path) {
  std::ofstream f(path, std::ios::trunc);
  if (!f)
    return;
  f << "reeot " REEOT_VERSION_STRING " " REEOT_GIT_COMMIT
       " (" REEOT_GIT_BRANCH ")\n";
  f << "platform " REEOT_BUILD_PLATFORM "\n";
  f << "built " REEOT_BUILD_TIMESTAMP "\n";
  f << std::format("history_seconds {}\n",
                   eot::Settings::Get().PerfHistorySeconds());
  f << std::format("ring_capacity {}\n", PerfCapacity());
}

void WriterLoop() {
  const auto dir = eot::AppRootFolder() / "logs" / "perf";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    EOT_WARN("[perf] cannot create {}: {}", dir.string(), ec.message());
    g_active.store(false, std::memory_order_relaxed);
    return;
  }

  const std::string stamp = FileTimestamp();
  const auto csv_path = dir / std::format("perf-{}.csv", stamp);
  WriteMeta(dir / std::format("perf-{}.meta.txt", stamp));

  std::ofstream f(csv_path, std::ios::binary | std::ios::trunc);
  if (!f) {
    EOT_WARN("[perf] cannot open {}", csv_path.string());
    g_active.store(false, std::memory_order_relaxed);
    return;
  }
  f << PerfCSVHeader() << '\n';
  EOT_INFO("[perf] capturing to {}", csv_path.string());

  std::vector<PerfSample> batch(kBatch);
  u64 cursor = PerfTotalPushed();
  std::string out;

  for (;;) {
    bool last = false;
    {
      std::unique_lock<std::mutex> lock(g_csv_mutex);
      g_cv.wait_for(lock, kDrainInterval, [] { return g_stopping; });
      last = g_stopping;
    }

    for (;;) {
      u64 next = 0, dropped = 0;
      const u32 n = PerfDrain(cursor, batch.data(), kBatch, &next, &dropped);
      if (dropped)
        g_dropped.fetch_add(dropped, std::memory_order_relaxed);
      cursor = next;
      if (!n)
        break;
      out.clear();
      for (u32 i = 0; i < n; ++i) {
        out += PerfCSVRow(batch[i]);
        out += '\n';
      }
      f.write(out.data(), std::streamsize(out.size()));
      if (!f) {
        EOT_WARN("[perf] csv write failed, stopping capture");
        g_active.store(false, std::memory_order_relaxed);
        return;
      }
      g_rows.fetch_add(n, std::memory_order_relaxed);
      if (n < kBatch)
        break;
    }

    if (last)
      break;
  }

  f.flush();
  EOT_INFO("[perf] capture closed: {} rows, {} dropped",
          g_rows.load(std::memory_order_relaxed),
          g_dropped.load(std::memory_order_relaxed));
  g_active.store(false, std::memory_order_relaxed);
}

void StartCapture() {
  std::lock_guard<std::mutex> life(g_lifecycle);
  if (g_thread.joinable())
    return;
  {
    std::lock_guard<std::mutex> lock(g_csv_mutex);
    g_stopping = false;
  }
  g_rows.store(0, std::memory_order_relaxed);
  g_dropped.store(0, std::memory_order_relaxed);
  g_active.store(true, std::memory_order_relaxed);
  g_thread = std::thread(WriterLoop);
}

void StopCapture() {
  std::lock_guard<std::mutex> life(g_lifecycle);
  if (!g_thread.joinable())
    return;
  {
    std::lock_guard<std::mutex> lock(g_csv_mutex);
    g_stopping = true;
  }
  g_cv.notify_all();
  g_thread.join();
  g_thread = std::thread();
  g_active.store(false, std::memory_order_relaxed);
}
}

std::string PerfCSVHeader() {
  std::string o;
#define X(member, col, kind)                                                   \
  if (!o.empty())                                                              \
    o += ',';                                                                  \
  o += col;
  EOT_PERF_FIELDS(X)
#undef X
  return o;
}

std::string PerfCSVRow(const PerfSample &s) {
  std::string o;
  o.reserve(320);
#define X(member, col, kind)                                                   \
  if (!o.empty())                                                              \
    o += ',';                                                                  \
  Append##kind(o, s.member);
  EOT_PERF_FIELDS(X)
#undef X
  return o;
}

void PerfConfigure(u32 capacity) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_ring.assign(capacity ? capacity : kDefaultCapacity, PerfSample{});
  g_written = 0;
}

u32 PerfCapacity() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return u32(g_ring.size());
}

u64 PerfTotalPushed() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_written;
}

void PerfPush(const PerfSample &s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_ring.empty())
    g_ring.assign(kDefaultCapacity, PerfSample{});
  g_ring[g_written % g_ring.size()] = s;
  ++g_written;
}

u32 PerfSnapshot(PerfSample *out, u32 max) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_ring.empty() || !out || !max)
    return 0;
  const u64 cap = g_ring.size();
  const u64 want = std::min<u64>(std::min<u64>(g_written, cap), max);
  const u64 first = g_written - want;
  for (u64 i = 0; i < want; ++i)
    out[i] = g_ring[(first + i) % cap];
  return u32(want);
}

u32 PerfDrain(u64 from_index, PerfSample *out, u32 max, u64 *out_next,
              u64 *out_dropped) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (out_dropped)
    *out_dropped = 0;
  if (g_ring.empty() || !out || !max) {
    if (out_next)
      *out_next = from_index;
    return 0;
  }
  const u64 cap = g_ring.size();
  const u64 oldest = g_written > cap ? g_written - cap : 0;
  if (from_index < oldest) {
    if (out_dropped)
      *out_dropped = oldest - from_index;
    from_index = oldest;
  }
  const u64 avail = g_written > from_index ? g_written - from_index : 0;
  const u64 want = std::min<u64>(avail, max);
  for (u64 i = 0; i < want; ++i)
    out[i] = g_ring[(from_index + i) % cap];
  if (out_next)
    *out_next = from_index + want;
  return u32(want);
}

PerfAggregate PerfSummarize(const PerfSample *s, u32 n) {
  PerfAggregate a;
  if (!s || !n)
    return a;
  std::vector<f64> dts;
  dts.reserve(n);
  f64 sum = 0.0, fence = 0.0, other = 0.0, pace = 0.0;
  for (u32 i = 0; i < n; ++i) {
    const f64 dt = s[i].dt_ms;
    dts.push_back(dt);
    sum += dt;
    fence += s[i].fence_ms;
    other += s[i].other_ms;
    pace += s[i].pace_ms;
    if (dt > a.max_ms)
      a.max_ms = dt;
    if (dt > 50.0)
      ++a.hitches;
  }
  a.frames = n;
  a.avg_ms = sum / n;
  a.fps = a.avg_ms > 0.0 ? 1000.0 / a.avg_ms : 0.0;
  std::sort(dts.begin(), dts.end());
  a.p99_ms = PercentileSorted(dts, 0.99);
  a.p999_ms = PercentileSorted(dts, 0.999);
  a.low1_fps = a.p99_ms > 0.0 ? 1000.0 / a.p99_ms : 0.0;
  a.low01_fps = a.p999_ms > 0.0 ? 1000.0 / a.p999_ms : 0.0;
  if (sum > 0.0) {
    a.fence_share = fence / sum;
    a.other_share = other / sum;
    a.pace_share = pace / sum;
  }
  return a;
}

PerfBound PerfClassify(const PerfAggregate &a) {
  if (!a.frames)
    return PerfBound::Unknown;
  const f64 f = a.fence_share, o = a.other_share, p = a.pace_share;
  const f64 top = std::max({f, o, p});
  if (top < 0.25)
    return PerfBound::Mixed;
  if (top == p)
    return PerfBound::Paced;
  if (top == f)
    return PerfBound::Gpu;
  return PerfBound::Cpu;
}

const char *PerfBoundName(PerfBound b) {
  switch (b) {
  case PerfBound::Cpu:
    return "CPU-BOUND";
  case PerfBound::Gpu:
    return "GPU-BOUND";
  case PerfBound::Paced:
    return "PACED";
  case PerfBound::Mixed:
    return "MIXED";
  default:
    return "---";
  }
}

void PerfCSVPoll() {
  static bool was_on = false;
  const bool on = eot::Settings::Get().PerfCSV();
  if (on == was_on)
    return;
  was_on = on;
  if (on)
    StartCapture();
  else
    StopCapture();
}

void PerfCSVShutdown() { StopCapture(); }

bool PerfCSVActive() { return g_active.load(std::memory_order_relaxed); }
u64 PerfCSVRowsWritten() { return g_rows.load(std::memory_order_relaxed); }
u64 PerfCSVRowsDropped() { return g_dropped.load(std::memory_order_relaxed); }

}
