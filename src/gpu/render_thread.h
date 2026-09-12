#pragma once

#include <mutex>

#include <rex/types.h>

#include "gpu/render_packets.h"

namespace plume {
struct RenderBuffer;
}

namespace eot::gpu {

enum class RenderCommandType : u32 {
  Draw,
  Clear,
  Resolve,
  Present,
  Unlock,
  Retire,
};

struct RenderCommand {
  RenderCommandType type = RenderCommandType::Draw;
  u64 seq = 0;
  u32 va = 0;
  plume::RenderBuffer *buffer = nullptr;
  ClearPacket clear;
  ResolvePacket resolve;
  DrawPacket draw;
};

void RenderThreadStart();
bool RenderThreadActive();
void RenderThreadStop();

class RenderEnqueue {
public:
  RenderEnqueue();
  ~RenderEnqueue();
  RenderEnqueue(const RenderEnqueue &) = delete;
  RenderEnqueue &operator=(const RenderEnqueue &) = delete;
  RenderCommand &cmd() { return *cmd_; }
  u64 commit();

private:
  std::unique_lock<std::mutex> lock_;
  RenderCommand *cmd_ = nullptr;
  bool committed_ = false;
};

void RenderThreadWait(u64 seq);

void RenderThreadUnlock(u32 resource_va);
void RenderThreadRetire(plume::RenderBuffer *buffer);

}
