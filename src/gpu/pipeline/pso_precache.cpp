#include "gpu/pipeline/pso_precache.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/settings.h"

namespace eot::gpu {

namespace {

struct WorkItem {
  PsoRecord rec;
  PsoSource source = PsoSource::Draw;
  TokenPtr token;
};

struct Pool {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<WorkItem> priority, background;
  std::vector<std::thread> threads;
  bool started = false, stop = false;

  std::mutex dedupMutex;
  std::unordered_map<u64, PsoSource> queuedOrDone;

  std::atomic<u32> queued{0}, built{0}, existing{0}, skipped{0}, failed{0};
};

Pool &pool() {
  static Pool p;
  return p;
}

thread_local TokenPtr t_loadToken;

void DemoteThread() {
#if defined(_WIN32)
  ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

void ProcessItem(WorkItem &item) {
  auto &p = pool();
  auto &s = state();
  switch (BuildPipelineFromRecord(s, item.rec, item.source)) {
  case PsoBuildResult::Built:
    p.built++;
    break;
  case PsoBuildResult::Existing:
    p.existing++;
    break;
  case PsoBuildResult::Skipped:
    p.skipped++;
    break;
  case PsoBuildResult::Failed:
    p.failed++;
    break;
  }
  if (item.token)
    item.token->ReleasePending();
}

void WorkerLoop() {
  DemoteThread();
  auto &p = pool();
  for (;;) {
    WorkItem item;
    {
      std::unique_lock lock(p.mutex);
      p.cv.wait(lock, [&] { return p.stop || !p.priority.empty() || !p.background.empty(); });
      if (p.stop && p.priority.empty() && p.background.empty())
        return;
      if (!p.priority.empty()) {
        item = std::move(p.priority.front());
        p.priority.pop_front();
      } else {
        item = std::move(p.background.front());
        p.background.pop_front();
      }
    }
    ProcessItem(item);
  }
}

}

void PsoPrecacheStart() {
  auto &p = pool();
  std::lock_guard lock(p.mutex);
  if (p.started)
    return;
  p.started = true;
  p.stop = false;
  const u32 hw = std::max(1u, std::thread::hardware_concurrency());
  u32 count = static_cast<u32>(Settings::PsoThreads());
  if (count == 0)
    count = std::clamp(hw > 2 ? hw - 2 : 1u, 1u, 8u);
  for (u32 i = 0; i < count; ++i)
    p.threads.emplace_back(WorkerLoop);
  EOT_INFO("[pso] {} pipeline worker thread(s)", count);
}

void PsoPrecacheStop() {
  auto &p = pool();
  std::vector<std::thread> threads;
  {
    std::lock_guard lock(p.mutex);
    if (!p.started)
      return;
    p.stop = true;
    for (auto &q : {&p.priority, &p.background}) {
      for (auto &item : *q)
        if (item.token)
          item.token->ReleasePending();
      q->clear();
    }
    threads.swap(p.threads);
  }
  p.cv.notify_all();
  for (auto &t : threads)
    if (t.joinable())
      t.join();
  std::lock_guard lock(p.mutex);
  p.started = false;
}

bool PsoPrecacheEnqueue(const PsoRecord &rec, PsoSource source, bool priority, TokenPtr token) {
  auto &p = pool();
  const u64 key = HashPipelineState(rec.state);
  {
    std::lock_guard lock(p.dedupMutex);
    if (!p.queuedOrDone.emplace(key, source).second)
      return false;
  }
  PsoPrecacheStart();
  if (token)
    token->AddPending();
  {
    std::lock_guard lock(p.mutex);
    if (p.stop)
      return false;
    (priority ? p.priority : p.background).push_back(WorkItem{rec, source, std::move(token)});
  }
  p.queued++;
  p.cv.notify_one();
  return true;
}

void PsoPrecacheBeginLoad() { t_loadToken = std::make_shared<CompileToken>(); }

TokenPtr PsoPrecacheCurrentToken() { return t_loadToken; }

bool PsoPrecacheWaitLoad(u32 max_ms) {
  TokenPtr token = t_loadToken;
  if (!token || token->Pending() == 0)
    return true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
  while (token->Pending() != 0) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

void PsoPrecacheEndLoad() { t_loadToken.reset(); }

bool PsoPrecacheKnown(u64 key, PsoSource *source) {
  auto &p = pool();
  std::lock_guard lock(p.dedupMutex);
  auto it = p.queuedOrDone.find(key);
  if (it == p.queuedOrDone.end())
    return false;
  if (source)
    *source = it->second;
  return true;
}

PsoPrecacheStats PsoPrecacheGetStats() {
  auto &p = pool();
  PsoPrecacheStats st;
  st.queued = p.queued.load();
  st.built = p.built.load();
  st.existing = p.existing.load();
  st.skipped = p.skipped.load();
  st.failed = p.failed.load();
  std::lock_guard lock(p.mutex);
  st.priorityPending = static_cast<u32>(p.priority.size());
  st.backgroundPending = static_cast<u32>(p.background.size());
  st.threads = static_cast<u32>(p.threads.size());
  return st;
}

}
