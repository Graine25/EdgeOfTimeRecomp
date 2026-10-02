#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <rex/types.h>

#include "gpu/pipeline/pipeline_cache.h"

namespace eot::gpu {

struct PsoRecord;

class CompileToken {
public:
  u32 Total() const { return total_.load(std::memory_order_acquire); }
  u32 Pending() const { return pending_.load(std::memory_order_acquire); }
  void AddPending() {
    pending_.fetch_add(1, std::memory_order_acq_rel);
    total_.fetch_add(1, std::memory_order_acq_rel);
  }
  void ReleasePending() { pending_.fetch_sub(1, std::memory_order_acq_rel); }

private:
  std::atomic<u32> pending_{0};
  std::atomic<u32> total_{0};
};
using TokenPtr = std::shared_ptr<CompileToken>;

enum class PsoLane : u8 { Recorded = 0, Derived = 1, Background = 2 };

void PsoPrecacheStart();
void PsoPrecacheStop();

void PsoPrecacheSetLoading(bool loading);
TokenPtr PsoPrecacheScreenToken();

bool PsoPrecacheEnqueue(const PsoRecord &rec, PsoSource source, PsoLane lane,
                        TokenPtr token = nullptr);

bool PsoPrecacheKnown(u64 key, PsoSource *source);

bool PsoPrecacheBuildNow(const PipelineState &st, u64 key);

void PsoPrecacheForget(const std::vector<u64> &keys);

struct PsoPrecacheStats {
  u32 queued = 0, built = 0, existing = 0, skipped = 0, failed = 0;
  u32 recordedPending = 0, derivedPending = 0, backgroundPending = 0, threads = 0;
  u32 urgentPending = 0, urgentBuilt = 0;
};
PsoPrecacheStats PsoPrecacheGetStats();

enum class PsoBuildResult { Built, Existing, Skipped, Failed };
PsoBuildResult BuildPipelineFromRecord(VideoState &s, const PsoRecord &rec, PsoSource source);

}
