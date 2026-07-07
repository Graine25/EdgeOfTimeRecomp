#include <atomic>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/guest/buffers.h"
#include "gpu/guest/d3d.h"

namespace eot::gpu {

namespace {

struct DrawStats {
  std::atomic<u32> vertices{0};
  std::atomic<u32> indexed{0};
  std::atomic<u32> no_stream{0};
  std::atomic<u32> no_index_bound{0};
  std::atomic<u32> no_index_base{0};
  std::atomic<u32> no_shaders{0};
  std::atomic<u32> translatable{0};
  std::atomic<u32> index32{0};
};
DrawStats g_draws;

void Classify(u32 device_va, bool indexed) {
  u32 addr = 0;
  u32 size = 0;
  const bool has_stream = ReadStreamFetch(device_va, 0, addr, size);
  if (!has_stream)
    g_draws.no_stream.fetch_add(1, std::memory_order_relaxed);

  bool has_indices = !indexed;
  if (indexed) {
    const u32 ib_va = mem::try_load<u32>(device_va + kDeviceIndexBufferShadow);
    GuestBuffer *ib =
        ib_va ? ResolveGuestBuffer(ib_va, ResourceType::IndexBuffer) : nullptr;
    if (!ib) {
      g_draws.no_index_bound.fetch_add(1, std::memory_order_relaxed);
    } else if (ib->address < 0x1000) {
      g_draws.no_index_base.fetch_add(1, std::memory_order_relaxed);
    } else {
      has_indices = true;
      if (ib->index32)
        g_draws.index32.fetch_add(1, std::memory_order_relaxed);
    }
  }

  const bool has_shaders = Video::BoundVertexShader() && Video::BoundPixelShader();
  if (!has_shaders)
    g_draws.no_shaders.fetch_add(1, std::memory_order_relaxed);

  if (has_stream && has_indices && has_shaders)
    g_draws.translatable.fetch_add(1, std::memory_order_relaxed);
}

}

void LogDrawStats() {
  const u32 total = g_draws.vertices.load() + g_draws.indexed.load();
  EOT_INFO("[draw] {} draws ({} indexed, {} of those 32-bit); {} translatable; "
           "dropped: {} no stream, {} no IB bound, {} IB has no base, {} no "
           "shaders",
           total, g_draws.indexed.load(), g_draws.index32.load(),
           g_draws.translatable.load(), g_draws.no_stream.load(),
           g_draws.no_index_bound.load(), g_draws.no_index_base.load(),
           g_draws.no_shaders.load());
}

namespace {

void CountDraw(u32 device_va, bool indexed) {
  auto &counter = indexed ? g_draws.indexed : g_draws.vertices;
  counter.fetch_add(1, std::memory_order_relaxed);
  Classify(device_va, indexed);
  const u32 total = g_draws.vertices.load(std::memory_order_relaxed) +
                    g_draws.indexed.load(std::memory_order_relaxed);
  if (total % 20000 == 0)
    LogDrawStats();
}

}
}

REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_HOOK_RAW(D3DDevice_DrawVertices) {
  const u32 device_va = ctx.r3.u32;
  __imp__D3DDevice_DrawVertices(ctx, base);
  eot::gpu::CountDraw(device_va, false);
}

REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  const u32 device_va = ctx.r3.u32;
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
  eot::gpu::CountDraw(device_va, true);
}
