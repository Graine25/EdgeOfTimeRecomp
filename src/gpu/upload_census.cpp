#include "gpu/upload_census.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace eot::gpu {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kForgetAfter = std::chrono::seconds(120);

std::mutex g_lock;
std::unordered_map<u32, Clock::time_point> g_resident;
std::unordered_set<u64> g_seen;
std::vector<u32> g_released;
std::deque<u32> g_preload;
std::unordered_set<u32> g_preload_wanted;
std::unordered_map<u64, u32> g_unannounced;

}

void NoteTextureResident(u32 header_va) {
  if (!header_va)
    return;
  const auto now = Clock::now();
  std::lock_guard<std::mutex> guard(g_lock);
  g_resident[header_va] = now;
  if (g_preload_wanted.insert(header_va).second)
    g_preload.push_back(header_va);
  if (g_resident.size() > 16384)
    std::erase_if(g_resident, [&](const auto &kv) { return now - kv.second > kForgetAfter; });
}

bool TakeTextureResidentAge(u32 header_va, f64 &age_ms) {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> guard(g_lock);
  const auto it = g_resident.find(header_va);
  if (it == g_resident.end())
    return false;
  const auto age = now - it->second;
  g_resident.erase(it);
  if (age > kForgetAfter)
    return false;
  age_ms = std::chrono::duration<f64, std::milli>(age).count();
  return true;
}

bool UploadSeenBefore(u64 storage_key) {
  std::lock_guard<std::mutex> guard(g_lock);
  return !g_seen.insert(storage_key).second;
}

void NoteTextureReleased(u32 header_va) {
  if (!header_va)
    return;
  std::lock_guard<std::mutex> guard(g_lock);
  g_resident.erase(header_va);
  g_preload_wanted.erase(header_va);
  if (g_released.size() < 65536)
    g_released.push_back(header_va);
}

bool TakeAnnouncedHeader(u32 &header_va) {
  std::lock_guard<std::mutex> guard(g_lock);
  while (!g_preload.empty()) {
    const u32 va = g_preload.front();
    g_preload.pop_front();
    if (g_preload_wanted.erase(va)) {
      header_va = va;
      return true;
    }
  }
  return false;
}

void TakeReleasedTextures(std::vector<u32> &out) {
  std::lock_guard<std::mutex> guard(g_lock);
  out.insert(out.end(), g_released.begin(), g_released.end());
  g_released.clear();
}

void NoteUnannouncedUpload(u32 width, u32 height, u32 guest_format, bool tiled) {
  const u64 key = (u64(width) << 40) | (u64(height) << 16) | (u64(guest_format) << 1) | (tiled ? 1 : 0);
  std::lock_guard<std::mutex> guard(g_lock);
  g_unannounced[key]++;
}

std::string TakeUnannouncedShapes() {
  std::vector<std::pair<u32, u64>> shapes;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    for (const auto &[key, count] : g_unannounced)
      shapes.emplace_back(count, key);
    g_unannounced.clear();
  }
  std::sort(shapes.begin(), shapes.end(), std::greater<>());
  std::string out;
  for (size_t i = 0; i < shapes.size() && i < 6; ++i) {
    const u64 key = shapes[i].second;
    out += std::format("{}{}x{} f{}{} x{}", out.empty() ? "" : ", ", u32(key >> 40), u32((key >> 16) & 0xFFFFFF),
                       u32((key >> 1) & 0x7FFF), (key & 1) ? "" : " linear", shapes[i].first);
  }
  return out;
}

}
