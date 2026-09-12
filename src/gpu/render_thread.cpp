#include "gpu/render_thread.h"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif
#include <immintrin.h>

#include "core/logging.h"
#include "core/profiling.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/settings.h"
#include "gpu/textures.h"

namespace eot::gpu {

namespace {

constexpr u64 kQueueSlots = 4096;
constexpr u64 kQueueMask = kQueueSlots - 1;
constexpr u64 kBatch = 64;
constexpr u32 kIdleSpins = 20000;

struct Queue {
  std::unique_ptr<RenderCommand[]> slots;
  alignas(64) std::atomic<u64> head{0};
  alignas(64) std::atomic<u64> tail{0};
  alignas(64) std::atomic<u64> executed{0};
  std::atomic<bool> consumer_sleeping{false};
  std::atomic<bool> producer_sleeping{false};
  std::atomic<bool> active{false};
  std::atomic<bool> stop{false};
  std::mutex produce_mutex;
  std::mutex cv_mutex;
  std::condition_variable cv_work, cv_space, cv_done;
  u64 next_seq = 1;
};

Queue &queue() {
  static auto *q = new Queue;
  return *q;
}

void PublishExecuted(Queue &q, u64 seq) {
  q.executed.store(seq, std::memory_order_release);
  std::lock_guard lock(q.cv_mutex);
  q.cv_done.notify_all();
}

void Prefetch(const RenderCommand &c) {
  const auto *base = reinterpret_cast<const char *>(&c);
  for (u32 off = 0; off < offsetof(RenderCommand, draw) + offsetof(DrawPacket, window); off += 64)
    _mm_prefetch(base + off, _MM_HINT_T0);
  const auto *image = reinterpret_cast<const char *>(c.draw.window.image);
  for (const DeviceWindow::Block &b : DeviceWindow::kBlocks)
    for (u32 off = 0; off < b.size; off += 64)
      _mm_prefetch(image + b.base + off, _MM_HINT_T0);
}

void Execute(VideoState &s, Queue &q, RenderCommand &c) {
  switch (c.type) {
  case RenderCommandType::Draw:
    ReplayDrawLocked(s, c.draw);
    break;
  case RenderCommandType::Clear:
    ReplayClearLocked(s, c.clear);
    break;
  case RenderCommandType::Resolve:
    ReplayResolveLocked(s, c.resolve);
    break;
  case RenderCommandType::Present:
    PresentLocked(s, c.va);
    PublishExecuted(q, c.seq);
    break;
  case RenderCommandType::Unlock:
    NotifyResourceUnlockedLocked(s, c.va);
    break;
  case RenderCommandType::Retire:
    ParkBuffer(s, std::unique_ptr<plume::RenderBuffer>(c.buffer));
    c.buffer = nullptr;
    break;
  }
}

u64 WaitForWork(Queue &q, u64 head) {
  for (u32 i = 0; i < kIdleSpins; ++i) {
    const u64 t = q.tail.load(std::memory_order_acquire);
    if (t != head || q.stop.load(std::memory_order_relaxed))
      return t;
    _mm_pause();
  }
  std::unique_lock lock(q.cv_mutex);
  q.consumer_sleeping.store(true, std::memory_order_seq_cst);
  q.cv_work.wait(lock, [&] {
    return q.tail.load(std::memory_order_seq_cst) != head || q.stop.load(std::memory_order_relaxed);
  });
  q.consumer_sleeping.store(false, std::memory_order_seq_cst);
  return q.tail.load(std::memory_order_acquire);
}

void WorkerMain() {
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"reeot render");
#endif
#if defined(EOT_PROFILING) && defined(REXGLUE_ENABLE_PROFILING)
  if (TracyIsStarted)
    tracy::SetThreadName("render");
#endif
  auto &q = queue();
  auto &s = state();
  u64 head = q.head.load(std::memory_order_relaxed);
  for (;;) {
    u64 tail;
    {
      PerfScope idle_scope(s.perf.worker_idle_ms);
      tail = WaitForWork(q, head);
    }
    if (q.stop.load(std::memory_order_acquire) || s.shutting_down.load(std::memory_order_acquire))
      break;
    EOT_CPU_ZONE("render batch");
    std::lock_guard lock(s.mutex);
    const u64 n = std::min<u64>(tail - head, kBatch);
    for (u64 i = 0; i < n; ++i) {
      RenderCommand &c = q.slots[head & kQueueMask];
      if (i + 1 < n)
        Prefetch(q.slots[(head + 1) & kQueueMask]);
      Execute(s, q, c);
      ++head;
      q.head.store(head, std::memory_order_seq_cst);
      if (q.producer_sleeping.load(std::memory_order_seq_cst)) {
        std::lock_guard cv_lock(q.cv_mutex);
        q.cv_space.notify_all();
      }
    }
  }
  q.active.store(false, std::memory_order_release);
  std::lock_guard lock(q.cv_mutex);
  q.cv_done.notify_all();
  q.cv_space.notify_all();
}

}

void RenderThreadStart() {
  auto &q = queue();
  if (q.active.load(std::memory_order_acquire) || !Settings::RenderThread())
    return;
  q.slots = std::make_unique<RenderCommand[]>(kQueueSlots);
  q.stop.store(false, std::memory_order_release);
  q.active.store(true, std::memory_order_release);
  std::thread(WorkerMain).detach();
  EOT_INFO("[gpu] render thread started: {} command slots ({} MB), batches of {}", kQueueSlots,
           (kQueueSlots * sizeof(RenderCommand)) >> 20, kBatch);
}

bool RenderThreadActive() { return queue().active.load(std::memory_order_acquire); }

void RenderThreadStop() {
  auto &q = queue();
  q.stop.store(true, std::memory_order_release);
  std::lock_guard lock(q.cv_mutex);
  q.cv_work.notify_all();
  q.cv_space.notify_all();
  q.cv_done.notify_all();
}

RenderEnqueue::RenderEnqueue() : lock_(queue().produce_mutex) {
  auto &q = queue();
  const u64 t = q.tail.load(std::memory_order_relaxed);
  if (t - q.head.load(std::memory_order_acquire) >= kQueueSlots) {
    EOT_CPU_ZONE("render queue full");
    std::unique_lock cv_lock(q.cv_mutex);
    q.producer_sleeping.store(true, std::memory_order_seq_cst);
    q.cv_space.wait(cv_lock, [&] {
      return t - q.head.load(std::memory_order_seq_cst) < kQueueSlots ||
             q.stop.load(std::memory_order_relaxed);
    });
    q.producer_sleeping.store(false, std::memory_order_seq_cst);
  }
  if (q.stop.load(std::memory_order_acquire)) {
    static RenderCommand scratch;
    cmd_ = &scratch;
    return;
  }
  cmd_ = &q.slots[t & kQueueMask];
  cmd_->seq = q.next_seq;
}

u64 RenderEnqueue::commit() {
  auto &q = queue();
  const u64 seq = q.next_seq++;
  if (q.stop.load(std::memory_order_acquire)) {
    committed_ = true;
    return seq;
  }
  cmd_->seq = seq;
  q.tail.store(q.tail.load(std::memory_order_relaxed) + 1, std::memory_order_seq_cst);
  if (q.consumer_sleeping.load(std::memory_order_seq_cst)) {
    std::lock_guard cv_lock(q.cv_mutex);
    q.cv_work.notify_one();
  }
  committed_ = true;
  return seq;
}

RenderEnqueue::~RenderEnqueue() = default;

void RenderThreadWait(u64 seq) {
  auto &q = queue();
  if (q.executed.load(std::memory_order_acquire) >= seq)
    return;
  std::unique_lock lock(q.cv_mutex);
  q.cv_done.wait(lock, [&] {
    return q.executed.load(std::memory_order_acquire) >= seq ||
           q.stop.load(std::memory_order_relaxed) || !q.active.load(std::memory_order_relaxed);
  });
}

void RenderThreadUnlock(u32 resource_va) {
  RenderEnqueue e;
  e.cmd().type = RenderCommandType::Unlock;
  e.cmd().va = resource_va;
  e.commit();
}

void RenderThreadRetire(plume::RenderBuffer *buffer) {
  if (!buffer)
    return;
  RenderEnqueue e;
  e.cmd().type = RenderCommandType::Retire;
  e.cmd().buffer = buffer;
  e.commit();
}

}
