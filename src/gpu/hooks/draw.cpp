#include <atomic>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/guest/buffers.h"
#include "gpu/guest/d3d.h"
#include "gpu/pipeline/constant_buffers.h"
#include "gpu/pipeline/pipeline_cache.h"

namespace eot::gpu {

namespace {

struct DrawStats {
  std::atomic<u32> vertices{0};
  std::atomic<u32> indexed{0};
  std::atomic<u32> no_stream{0};
  std::atomic<u32> no_index_bound{0};
  std::atomic<u32> no_index_base{0};
  std::atomic<u32> no_shaders{0};
  std::atomic<u32> stage_unresolved{0};
  std::atomic<u32> depth_only{0};
  std::atomic<u32> framebuffer_ok{0};
  std::atomic<u32> fb_not_ready{0};
  std::atomic<u32> fb_nothing_bound{0};
  std::atomic<u32> fb_depth_only{0};
  std::atomic<u32> fb_no_host_texture{0};
  std::atomic<u32> fb_create_failed{0};
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
      if (g_draws.no_index_base.fetch_add(1, std::memory_order_relaxed) < 3) {
        const auto *raw = eot::mem::try_at<const eot::be<u32>>(ib_va);
        EOT_WARN("[draw] IB 0x{:08X} base=0 size={} hdr = {:08X} {:08X} "
                 "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 ib_va, ib->size, raw ? u32(raw[0]) : 0, raw ? u32(raw[1]) : 0,
                 raw ? u32(raw[2]) : 0, raw ? u32(raw[3]) : 0,
                 raw ? u32(raw[4]) : 0, raw ? u32(raw[5]) : 0,
                 raw ? u32(raw[6]) : 0, raw ? u32(raw[7]) : 0);
      }
    } else {
      has_indices = true;
      if (ib->index32)
        g_draws.index32.fetch_add(1, std::memory_order_relaxed);
    }
  }

  const u32 vs_va = mem::try_load<u32>(device_va + kDeviceVertexShaderShadow);
  const u32 ps_va = mem::try_load<u32>(device_va + kDevicePixelShaderShadow);
  const bool has_vs = Video::BoundVertexShader() != nullptr;
  const bool has_ps = Video::BoundPixelShader() != nullptr;
  if (has_vs && !has_ps)
    g_draws.depth_only.fetch_add(1, std::memory_order_relaxed);
  if (!has_vs) {
    g_draws.no_shaders.fetch_add(1, std::memory_order_relaxed);
    if (vs_va && g_draws.stage_unresolved.fetch_add(
                     1, std::memory_order_relaxed) < 3) {
      EOT_WARN("[draw] VS bound but unresolved: vs=0x{:08X} ps=0x{:08X}", vs_va,
               ps_va);
    }
  }

  if (!(has_stream && has_indices && has_vs))
    return;
  g_draws.translatable.fetch_add(1, std::memory_order_relaxed);

  switch (Video::BindDrawFramebuffer()) {
  case Video::FramebufferBind::kBound:
    g_draws.framebuffer_ok.fetch_add(1, std::memory_order_relaxed);
    break;
  case Video::FramebufferBind::kNotReady:
    g_draws.fb_not_ready.fetch_add(1, std::memory_order_relaxed);
    break;
  case Video::FramebufferBind::kNothingBound:
    g_draws.fb_nothing_bound.fetch_add(1, std::memory_order_relaxed);
    break;
  case Video::FramebufferBind::kDepthOnly:
    g_draws.fb_depth_only.fetch_add(1, std::memory_order_relaxed);
    break;
  case Video::FramebufferBind::kNoHostTexture:
    g_draws.fb_no_host_texture.fetch_add(1, std::memory_order_relaxed);
    break;
  case Video::FramebufferBind::kCreateFailed:
    g_draws.fb_create_failed.fetch_add(1, std::memory_order_relaxed);
    break;
  }

  PipelineKey key;
  plume::RenderPipeline *pipeline = nullptr;
  if (BuildPipelineKeyForCurrentState(device_va, key))
    pipeline = GetOrCreatePipeline(key, key.layout);
  else
    NotePipelineUndescribable();

  if (pipeline)
    constants::UploadDrawConstants(device_va);
}

}

void LogDrawStats() {
  const u32 total = g_draws.vertices.load() + g_draws.indexed.load();
  EOT_INFO("[draw] {} draws ({} indexed, {} of those 32-bit); {} translatable; "
           "dropped: {} no stream, {} no IB bound, {} IB has no base, {} no "
           "vertex shader ({} unresolved); {} depth-only; framebuffer {} ok, "
           "{} not-ready, {} nothing-bound, {} depth-only, {} no-host-texture, "
           "{} create-failed",
           total, g_draws.indexed.load(), g_draws.index32.load(),
           g_draws.translatable.load(), g_draws.no_stream.load(),
           g_draws.no_index_bound.load(), g_draws.no_index_base.load(),
           g_draws.no_shaders.load(), g_draws.stage_unresolved.load(),
           g_draws.depth_only.load(), g_draws.framebuffer_ok.load(),
           g_draws.fb_not_ready.load(), g_draws.fb_nothing_bound.load(),
           g_draws.fb_depth_only.load(), g_draws.fb_no_host_texture.load(),
           g_draws.fb_create_failed.load());
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
