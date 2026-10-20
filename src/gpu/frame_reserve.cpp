// gpu/frame_reserve.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include "gpu/frame_reserve.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/logging.h"
#include "gpu/memory_report.h"
#include "gpu/pipeline/pso_records.h"
#include "gpu/settings.h"

namespace eot::gpu {

namespace {

constexpr const char *kFrameSetFile = "eot_frame_set.tsv";
constexpr u32 kMaxPerShape = 16;
constexpr u32 kMaxShapes = 96;
constexpr auto kUnclaimedAfter = std::chrono::seconds(90);
// One image a refresh at most, so a boot movie never meets two creations in one interval.
constexpr auto kPace = std::chrono::milliseconds(20);

// Everything a RenderTextureDesc says, the clear value included.
struct ShapeKey {
  std::array<u32, 16> v{};
  bool operator==(const ShapeKey &o) const { return v == o.v; }
};

ShapeKey KeyOf(const plume::RenderTextureDesc &d) {
  ShapeKey k;
  k.v[0] = static_cast<u32>(d.dimension);
  k.v[1] = d.width;
  k.v[2] = d.height;
  k.v[3] = d.depth;
  k.v[4] = d.mipLevels;
  k.v[5] = d.arraySize;
  k.v[6] = static_cast<u32>(d.multisampling.sampleCount);
  k.v[7] = static_cast<u32>(d.format);
  k.v[8] = static_cast<u32>(d.textureArrangement);
  k.v[9] = static_cast<u32>(d.flags);
  k.v[10] = d.committed ? 1u : 0u;
  if (const plume::RenderClearValue *c = d.optimizedClearValue) {
    k.v[11] = 1u;
    k.v[12] = static_cast<u32>(c->format);
    u32 bits[4] = {};
    static_assert(sizeof(c->color) == sizeof(bits));
    std::memcpy(bits, &c->color, sizeof(bits));
    k.v[13] = bits[0];
    k.v[14] = bits[1];
    k.v[15] = bits[2] ^ (bits[3] * 0x9E3779B9u);
  }
  return k;
}

struct Shape {
  ShapeKey key;
  plume::RenderTextureDesc desc;
  plume::RenderClearValue clear;
  std::string tag;
  u32 count = 0;
  std::vector<std::unique_ptr<plume::RenderTexture>> made;
};

struct Reserve {
  std::mutex mutex;
  std::vector<Shape> shapes;
  std::vector<Shape> noted;
  bool dirty = false;
  std::thread thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> housekeeping{false};
  plume::RenderDevice *device = nullptr;
  bool started = false;
  bool released = false;
  u32 made = 0, taken = 0;
  u64 madeBytes = 0;
  std::chrono::steady_clock::time_point firstTake{};
};

Reserve &reserve() {
  static Reserve r;
  return r;
}

std::string FilePath() { return (std::filesystem::path(PsoDir()) / kFrameSetFile).string(); }

u64 ApproxBytes(const plume::RenderTextureDesc &d) {
  return u64(d.width) * std::max(d.height, 1u) * std::max({d.depth, d.arraySize, 1u}) *
         std::max(static_cast<u32>(d.multisampling.sampleCount), 1u) * 4u;
}

// The frame's own images: surfaces, and mirrors and TAA images at the frame's size or a power-of-two
// square (shadow tiles). Mirrors of a scaled copy come in a new size each frame and are left out.
bool FrameSetImage(const plume::RenderTextureDesc &d, const char *tag) {
  if (!tag)
    return false;
  const std::string_view t(tag);
  if (t.starts_with("surface"))
    return true;
  if (!t.starts_with("resolve-mirror") && !t.starts_with("taa") && !t.starts_with("velocity"))
    return false;
  const u32 w = InternalRenderWidth(), h = InternalRenderHeight();
  const bool frame_sized = d.width == w && d.height >= h && d.height <= h + 32;
  const bool shadow_tile = d.width == d.height && (d.width & (d.width - 1)) == 0;
  return frame_sized || shadow_tile;
}

void FillDesc(Shape &s) {
  const auto &v = s.key.v;
  s.desc = {};
  s.desc.dimension = static_cast<plume::RenderTextureDimension>(v[0]);
  s.desc.width = v[1];
  s.desc.height = v[2];
  s.desc.depth = v[3];
  s.desc.mipLevels = v[4];
  s.desc.arraySize = v[5];
  s.desc.multisampling.sampleCount = static_cast<plume::RenderSampleCounts>(v[6]);
  s.desc.format = static_cast<plume::RenderFormat>(v[7]);
  s.desc.textureArrangement = static_cast<plume::RenderTextureArrangement>(v[8]);
  s.desc.flags = static_cast<plume::RenderTextureFlags>(v[9]);
  s.desc.committed = v[10] != 0;
  s.desc.optimizedClearValue = nullptr;
}

// A line a shape: tag, count, the sixteen key words, and the clear colour's four words.
std::vector<Shape> ReadShapes() {
  std::vector<Shape> out;
  FILE *f = std::fopen(FilePath().c_str(), "rb");
  if (!f)
    return out;
  char line[1024];
  while (std::fgets(line, sizeof(line), f) && out.size() < kMaxShapes) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
      continue;
    Shape s;
    char tag[64] = {};
    u32 c[4] = {};
    auto &v = s.key.v;
    const int n = std::sscanf(line,
                              "%63s %u %u %u %u %u %u %u %u %u %u %u %u %u %x %x %x %x %x %x %x %x",
                              tag, &s.count, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                              &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15], &c[0],
                              &c[1], &c[2], &c[3]);
    if (n != 22 || s.count == 0 || s.count > kMaxPerShape || v[1] == 0)
      continue;
    s.tag = tag;
    FillDesc(s);
    if (!FrameSetImage(s.desc, tag))
      continue;
    if (v[11]) {
      s.clear.format = static_cast<plume::RenderFormat>(v[12]);
      std::memcpy(&s.clear.color, c, sizeof(c));
      plume::RenderTextureDesc d = s.desc;
      d.optimizedClearValue = &s.clear;
      if (KeyOf(d) != s.key)
        continue;
    }
    out.push_back(std::move(s));
  }
  std::fclose(f);
  return out;
}

plume::RenderTextureDesc DescOf(const Shape &s) {
  plume::RenderTextureDesc d = s.desc;
  d.optimizedClearValue = s.key.v[11] ? &s.clear : nullptr;
  return d;
}

void MakerLoop() {
  auto &r = reserve();
  size_t shape = 0;
  u32 copy = 0;
  for (;;) {
    if (r.stop.load(std::memory_order_acquire))
      return;
    plume::RenderTextureDesc desc;
    std::string tag;
    {
      std::lock_guard lock(r.mutex);
      while (shape < r.shapes.size() && copy >= r.shapes[shape].count) {
        ++shape;
        copy = 0;
      }
      if (shape >= r.shapes.size() || r.released)
        break;
      desc = DescOf(r.shapes[shape]);
      tag = r.shapes[shape].tag;
      ++copy;
    }
    auto texture = r.device->createTexture(desc);
    if (texture) {
      TagHostAllocation(texture.get(), tag.c_str(), desc);
      std::lock_guard lock(r.mutex);
      r.shapes[shape].made.push_back(std::move(texture));
      r.made++;
      r.madeBytes += ApproxBytes(desc);
    }
    std::this_thread::sleep_for(kPace);
  }
  std::lock_guard lock(r.mutex);
  EOT_INFO("[reserve] {} frame image(s) made ahead, about {:.0f} MB", r.made, r.madeBytes / (1024.0 * 1024.0));
}

}

void FrameReserveStart(plume::RenderDevice *device) {
  auto &r = reserve();
  std::lock_guard lock(r.mutex);
  if (r.started || !device)
    return;
  r.started = true;
  r.device = device;
  if (const char *e = std::getenv("EOT_FRAME_RESERVE"); e && e[0] == '0') {
    EOT_INFO("[reserve] off (EOT_FRAME_RESERVE=0); shapes are still recorded");
    return;
  }
  r.shapes = ReadShapes();
  if (r.shapes.empty())
    return;
  u32 images = 0;
  for (const Shape &s : r.shapes)
    images += s.count;
  EOT_INFO("[reserve] {}: {} shape(s), {} image(s) to make behind the boot", FilePath(), r.shapes.size(), images);
  r.thread = std::thread(MakerLoop);
}

std::unique_ptr<plume::RenderTexture> FrameReserveTake(const plume::RenderTextureDesc &desc) {
  auto &r = reserve();
  std::lock_guard lock(r.mutex);
  if (r.shapes.empty())
    return nullptr;
  const ShapeKey key = KeyOf(desc);
  for (Shape &s : r.shapes) {
    if (s.made.empty() || !(s.key == key))
      continue;
    auto texture = std::move(s.made.back());
    s.made.pop_back();
    if (r.taken++ == 0)
      r.firstTake = std::chrono::steady_clock::now();
    return texture;
  }
  return nullptr;
}

void FrameReserveNote(const plume::RenderTextureDesc &desc, const char *tag) {
  if (!FrameSetImage(desc, tag) || ApproxBytes(desc) < 4ull << 20)
    return;
  auto &r = reserve();
  std::lock_guard lock(r.mutex);
  const ShapeKey key = KeyOf(desc);
  for (Shape &s : r.noted) {
    if (s.key == key) {
      if (s.count < kMaxPerShape) {
        s.count++;
        r.dirty = true;
      }
      return;
    }
  }
  if (r.noted.size() >= kMaxShapes)
    return;
  Shape s;
  s.key = key;
  s.tag = tag;
  s.count = 1;
  if (desc.optimizedClearValue)
    s.clear = *desc.optimizedClearValue;
  FillDesc(s);
  r.noted.push_back(std::move(s));
  r.dirty = true;
}

void FrameReserveHousekeep(bool shutdown) {
  auto &r = reserve();
  std::vector<std::unique_ptr<plume::RenderTexture>> unclaimed;
  std::string text;
  u32 taken = 0;
  {
    std::lock_guard lock(r.mutex);
    taken = r.taken;
    const bool late = r.taken > 0 && std::chrono::steady_clock::now() - r.firstTake > kUnclaimedAfter;
    if (!r.released && (late || shutdown)) {
      r.released = true;
      for (Shape &s : r.shapes)
        for (auto &t : s.made)
          unclaimed.push_back(std::move(t));
      for (Shape &s : r.shapes)
        s.made.clear();
    }
    if (r.dirty) {
      r.dirty = false;
      // The union of what the file held and this session's shapes, at the larger count, so a
      // session at the title keeps a level's shapes.
      std::vector<const Shape *> all;
      for (const Shape &s : r.noted)
        all.push_back(&s);
      for (const Shape &s : r.shapes) {
        const bool seen =
            std::any_of(r.noted.begin(), r.noted.end(), [&](const Shape &n) { return n.key == s.key; });
        if (!seen)
          all.push_back(&s);
      }
      text = "# eot frame set: the images the next boot makes ahead\n";
      for (const Shape *s : all) {
        u32 count = s->count;
        for (const Shape &o : r.shapes)
          if (o.key == s->key)
            count = std::max(count, o.count);
        u32 c[4] = {};
        if (s->key.v[11])
          std::memcpy(c, &s->clear.color, sizeof(c));
        char line[512];
        const auto &v = s->key.v;
        std::snprintf(line, sizeof(line), "%s %u %u %u %u %u %u %u %u %u %u %u %u %u %x %x %x %x %x %x %x %x\n",
                      s->tag.c_str(), count, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10],
                      v[11], v[12], v[13], v[14], v[15], c[0], c[1], c[2], c[3]);
        text += line;
      }
    }
  }
  if (!unclaimed.empty())
    EOT_INFO("[reserve] {} image(s) made ahead were not taken and are released ({} taken)", unclaimed.size(),
             taken);
  unclaimed.clear();
  if (text.empty())
    return;
  std::error_code ec;
  std::filesystem::create_directories(PsoDir(), ec);
  const std::string path = FilePath(), tmp = path + ".tmp";
  if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (ok)
      std::filesystem::rename(tmp, path, ec);
  }
}

void FrameReserveHousekeepAsync() {
  auto &r = reserve();
  if (r.housekeeping.exchange(true, std::memory_order_acq_rel))
    return;
  std::thread([] {
    FrameReserveHousekeep(false);
    reserve().housekeeping.store(false, std::memory_order_release);
  }).detach();
}

void FrameReserveStop() {
  auto &r = reserve();
  r.stop.store(true, std::memory_order_release);
  if (r.thread.joinable())
    r.thread.join();
}

}
