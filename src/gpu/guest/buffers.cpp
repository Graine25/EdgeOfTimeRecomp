#include "gpu/guest/buffers.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/host_resource_heap.h"
#include "gpu/guest/d3d.h"

namespace eot::gpu {

namespace {

std::mutex g_buffer_mutex;
std::unordered_map<u32, GuestBuffer *> g_buffers;

struct Stats {
  std::atomic<u32> vertex{0};
  std::atomic<u32> index{0};
  std::atomic<u32> stamped{0};
  std::atomic<u32> at_bind{0};
  std::atomic<u32> zero_base{0};
  std::atomic<u32> malformed{0};
};
Stats g_stats;

const char *TypeName(ResourceType type) {
  return type == ResourceType::IndexBuffer ? "IB" : "VB";
}

bool RefreshLocked(GuestBuffer &buf) {
  if (buf.type == ResourceType::IndexBuffer) {
    const auto *ib = mem::try_at<const D3DIndexBuffer>(buf.headerVa);
    if (!ib)
      return false;
    buf.address = ib->Address;
    buf.size = ib->Size;
    buf.indexFormat = IndexBufferFormat(*ib);
  } else {
    const auto *vb = mem::try_at<const D3DVertexBuffer>(buf.headerVa);
    if (!vb)
      return false;
    buf.address = VertexBufferAddress(*vb);
    buf.size = VertexBufferSize(*vb);
  }
  return true;
}

GuestBuffer *Publish(u32 header_va, ResourceType type, bool from_stamp) {
  auto *buf = HostResourceHeap::Alloc<GuestBuffer>(type);
  if (!buf)
    return nullptr;
  buf->headerVa = header_va;

  std::lock_guard lock(g_buffer_mutex);
  auto [it, inserted] = g_buffers.try_emplace(header_va, buf);
  if (!inserted) {
    HostResourceHeap::Free(buf);
    return it->second;
  }
  if (!RefreshLocked(*buf)) {
    g_buffers.erase(it);
    HostResourceHeap::Free(buf);
    g_stats.malformed.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  (from_stamp ? g_stats.stamped : g_stats.at_bind)
      .fetch_add(1, std::memory_order_relaxed);
  auto &kind =
      type == ResourceType::IndexBuffer ? g_stats.index : g_stats.vertex;
  if (kind.fetch_add(1, std::memory_order_relaxed) == 0) {
    EOT_INFO("[buffer] first {} header 0x{:08X}: address=0x{:08X} size={} ({})",
             TypeName(type), header_va, buf->address, buf->size,
             from_stamp ? "stamped" : "at bind");
  }
  const u32 total = g_stats.vertex.load(std::memory_order_relaxed) +
                    g_stats.index.load(std::memory_order_relaxed);
  if (total == 100 || total % 1000 == 0)
    LogBufferStats();
  return buf;
}

}

GuestBuffer *RegisterBufferHeader(u32 header_va, ResourceType type) {
  if (!header_va)
    return nullptr;
  {
    std::lock_guard lock(g_buffer_mutex);
    auto it = g_buffers.find(header_va);
    if (it != g_buffers.end()) {
      it->second->type = type;
      RefreshLocked(*it->second);
      return it->second;
    }
  }
  return Publish(header_va, type, true);
}

GuestBuffer *ResolveGuestBuffer(u32 header_va, ResourceType type) {
  if (!header_va)
    return nullptr;
  GuestBuffer *buf = nullptr;
  {
    std::lock_guard lock(g_buffer_mutex);
    auto it = g_buffers.find(header_va);
    if (it != g_buffers.end()) {
      buf = it->second;
      RefreshLocked(*buf);
    }
  }
  if (!buf)
    buf = Publish(header_va, type, false);
  if (!buf)
    return nullptr;

  if (buf->address == 0 && !buf->reportedEmpty) {
    buf->reportedEmpty = true;
    g_stats.zero_base.fetch_add(1, std::memory_order_relaxed);
  }
  return buf;
}

void LogBufferStats() {
  EOT_INFO("[buffer] {} vertex, {} index; {} first seen at stamp, {} at bind; "
           "{} with a zero base, {} malformed",
           g_stats.vertex.load(), g_stats.index.load(), g_stats.stamped.load(),
           g_stats.at_bind.load(), g_stats.zero_base.load(),
           g_stats.malformed.load());
}

}
