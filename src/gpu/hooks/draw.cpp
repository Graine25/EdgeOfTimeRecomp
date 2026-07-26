#include <algorithm>
#include <atomic>
#include <string>
#include <cstdlib>
#include <mutex>
#include <set>

#include <rex/hook.h>
#include <rex/types.h>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/settings.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/guest/buffers.h"
#include "gpu/guest/immediate.h"
#include "gpu/guest/d3d.h"
#include "gpu/device/native_texture_mirror.h"
#include "gpu/pipeline/constant_buffers.h"
#include "gpu/guest/texture_fetch.h"
#include "gpu/pipeline/geometry_upload.h"
#include "gpu/guest/vfetch_microcode.h"
#include "gpu/pipeline/pipeline_cache.h"

REXCVAR_DEFINE_BOOL(eot_immediate_draws, true, "gpu",
                    "Issue geometry from D3DDevice_BeginVertices. Each draw's "
                    "layout is read from the shader's vfetch microcode and "
                    "skipped if it disagrees with the stride the guest passed.");

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

struct DrawArgs {
  u32 primitiveType = 0;
  u32 startVertex = 0;
  u32 vertexCount = 0;
  u32 baseVertexIndex = 0;
  u32 startIndex = 0;
  u32 indexCount = 0;
};

std::atomic<u32> g_issued{0};
std::atomic<bool> g_frame_clear_pending{false};
std::atomic<u32> g_no_geometry{0};
std::atomic<u32> g_no_list{0};
std::atomic<u32> g_no_framebuffer{0};

void IssueDraw(u32 device_va, const PipelineKey &key,
               plume::RenderPipeline *pipeline, bool indexed,
               const DrawArgs &args) {
  auto *layout_obj = Video::GuestPipelineLayout();
  auto *texture_set = Video::GuestTextureSet();
  auto *sampler_set = Video::GuestSamplerSet();
  if (!pipeline || !layout_obj || !texture_set || !sampler_set)
    return;

  const constants::DrawConstants cb =
      constants::UploadDrawConstants(device_va, &key.layout);
  if (!cb.valid())
    return;

  GuestShader *vs = Video::BoundVertexShader();
  const bool window_space = vs && vs->shaderCacheEntry &&
                            vs->shaderCacheEntry->uses_float_constants == 0;
  const auto target = Video::BoundAttachmentSize();

  plume::RenderViewport viewport(0.0f, 0.0f, float(target.width),
                                 float(target.height));
  {
    const u32 at = device_va + kViewportOffset;
    const float x = eot::mem::try_load<float>(at);
    const float y = eot::mem::try_load<float>(at + 4);
    const float w = eot::mem::try_load<float>(at + 8);
    const float h = eot::mem::try_load<float>(at + 12);
    const float min_z = eot::mem::try_load<float>(at + 16);
    const float max_z = eot::mem::try_load<float>(at + 20);
    if (w > 0.0f && h > 0.0f && x >= 0.0f && y >= 0.0f &&
        x + w <= float(target.width) + 1.0f &&
        y + h <= float(target.height) + 1.0f) {
      viewport = plume::RenderViewport(x, y, w, h, min_z, max_z);
    }
  }

  const u32 space_w = u32(viewport.width);
  const u32 space_h = u32(viewport.height);

  DrawGeometry geometry;
  if (!UploadDrawGeometry(key.layout, indexed ? 0 : args.startVertex,
                          args.vertexCount, indexed, args.startIndex,
                          args.indexCount, args.baseVertexIndex,
                          args.primitiveType, window_space, space_w, space_h,
                          geometry)) {
    g_no_geometry.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (Video::BindDrawFramebuffer() != Video::FramebufferBind::kBound) {
    g_no_framebuffer.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  auto rec = Video::AcquireRecordingList();
  if (!rec || !rec.framebuffer) {
    g_no_list.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  plume::RenderTextureBarrier to_write[2];
  u32 barrier_count = 0;
  if (rec.colorTarget && rec.colorTarget->texture &&
      rec.colorTarget->layout != plume::RenderTextureLayout::COLOR_WRITE) {
    to_write[barrier_count++] = plume::RenderTextureBarrier(
        rec.colorTarget->texture, plume::RenderTextureLayout::COLOR_WRITE);
    rec.colorTarget->layout = plume::RenderTextureLayout::COLOR_WRITE;
  }
  if (rec.depthTarget && rec.depthTarget->texture &&
      rec.depthTarget->layout != plume::RenderTextureLayout::DEPTH_WRITE) {
    to_write[barrier_count++] = plume::RenderTextureBarrier(
        rec.depthTarget->texture, plume::RenderTextureLayout::DEPTH_WRITE);
    rec.depthTarget->layout = plume::RenderTextureLayout::DEPTH_WRITE;
  }
  if (barrier_count)
    rec.cmd->barriers(plume::RenderBarrierStage::GRAPHICS, to_write,
                      barrier_count);

  rec.cmd->setFramebuffer(rec.framebuffer);

  float clear_z = 0.0f;
  u32 clear_stencil = 0;
  bool clear_depth = true;
  if (Video::TakePendingDepthClear(rec, clear_z, clear_stencil, clear_depth))
    rec.cmd->clearDepthStencil(clear_depth, true, clear_z, clear_stencil);

  rec.cmd->setPipeline(pipeline);
  rec.cmd->setGraphicsPipelineLayout(layout_obj);
  rec.cmd->setGraphicsDescriptorSet(texture_set, 0);
  rec.cmd->setGraphicsDescriptorSet(texture_set, 1);
  rec.cmd->setGraphicsDescriptorSet(texture_set, 2);
  rec.cmd->setGraphicsDescriptorSet(sampler_set, 3);
  rec.cmd->setGraphicsDescriptorSet(texture_set, 4);
  rec.cmd->setGraphicsRootDescriptor(cb.vs.ref, 0);
  rec.cmd->setGraphicsRootDescriptor(cb.ps.ref, 1);
  rec.cmd->setGraphicsRootDescriptor(cb.shared.ref, 2);
  rec.cmd->setVertexBuffers(0, geometry.vertexViews, geometry.vertexBufferCount,
                            geometry.vertexSlots);
  Video::NoteReducedViewportDraw(rec, u32(viewport.width),
                                 u32(viewport.height));
  rec.cmd->setViewports(viewport);
  {
    const u32 tl = eot::mem::try_load<u32>(device_va + kWindowScissorTLOffset);
    const u32 br = eot::mem::try_load<u32>(device_va + kWindowScissorBROffset);
    const u32 left = std::min(ScissorX(tl), rec.targetWidth);
    const u32 top = std::min(ScissorY(tl), rec.targetHeight);
    const u32 right = std::min(ScissorX(br), rec.targetWidth);
    const u32 bottom = std::min(ScissorY(br), rec.targetHeight);
    if (right > left && bottom > top) {
      rec.cmd->setScissors(plume::RenderRect(i32(left), i32(top), i32(right),
                                             i32(bottom)));
    } else {
      rec.cmd->setScissors(plume::RenderRect(0, 0, i32(rec.targetWidth),
                                             i32(rec.targetHeight)));
    }
  }

  if (geometry.hasIndices) {
    rec.cmd->setIndexBuffer(&geometry.indexView);
    rec.cmd->drawIndexedInstanced(geometry.indexCount, 1, 0, 0, 0);
  } else {
    rec.cmd->drawInstanced(args.vertexCount, 1, 0, 0);
  }

  Video::NoteAttachmentsDrawnLocked(rec.colorTarget, rec.depthTarget);

  if (g_issued.fetch_add(1, std::memory_order_relaxed) == 0)
    EOT_INFO("[draw] first host draw issued: {} {} into {}x{}",
             indexed ? args.indexCount : args.vertexCount,
             indexed ? "indices" : "vertices", rec.targetWidth,
             rec.targetHeight);
}

void Classify(u32 device_va, bool indexed, const DrawArgs &args) {
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
    IssueDraw(device_va, key, pipeline, indexed, args);
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

void CountDraw(u32 device_va, bool indexed, const DrawArgs &args) {
  FlushImmediateVertices();
  auto &counter = indexed ? g_draws.indexed : g_draws.vertices;
  counter.fetch_add(1, std::memory_order_relaxed);
  Classify(device_va, indexed, args);
  const u32 total = g_draws.vertices.load(std::memory_order_relaxed) +
                    g_draws.indexed.load(std::memory_order_relaxed);
  if (total % 20000 == 0) {
    LogDrawStats();
    EOT_INFO("[draw] {} host draws issued; skipped: {} no geometry, "
             "{} no framebuffer, {} no open list",
             g_issued.load(), g_no_geometry.load(), g_no_framebuffer.load(),
             g_no_list.load());
    LogGeometryUploadStats();
    LogNativeTextureStats();
    Video::LogResolveStats();
  }
}

}

namespace {

struct PendingImmediate {
  u32 device_va = 0;
  u32 primitiveType = 0;
  u32 vertexCount = 0;
  u32 stride = 0;
  u32 address = 0;
  GuestShader *shader = nullptr;
};
std::mutex g_immediate_mutex;
PendingImmediate g_immediate;
std::atomic<u32> g_immediate_seen{0};
std::atomic<u32> g_immediate_issued{0};
std::atomic<u32> g_immediate_unlayouted{0};

}

void NoteImmediateVertices(u32 device_va, u32 primitiveType, u32 vertexCount,
                           u32 stride, u32 address) {
  FlushImmediateVertices();
  if (!device_va || !vertexCount || !stride || !address)
    return;
  std::lock_guard lock(g_immediate_mutex);
  g_immediate = {device_va,  primitiveType,           vertexCount,
                 stride,     address,                 Video::BoundVertexShader()};
  if (g_immediate_seen.fetch_add(1, std::memory_order_relaxed) < 4) {
    EOT_INFO("[immediate] BeginVertices prim={} count={} stride={} at 0x{:08X}",
             primitiveType, vertexCount, stride, address);
  }
}

void FlushImmediateVertices() {
  if (!REXCVAR_GET(eot_immediate_draws)) {
    std::lock_guard lock(g_immediate_mutex);
    g_immediate = {};
    return;
  }
  PendingImmediate pending;
  {
    std::lock_guard lock(g_immediate_mutex);
    if (!g_immediate.address)
      return;
    pending = g_immediate;
    g_immediate = {};
  }

  Video::SetImmediateStream(pending.address, pending.stride,
                            pending.vertexCount * pending.stride);

  if (Video::BindDrawFramebuffer() == Video::FramebufferBind::kBound) {
    PipelineKey key;
    if (BuildPipelineKeyForCurrentState(pending.device_va, key)) {
      GuestShader *vs = pending.shader;
      VertexLayout fetches;
      if (!vs || !DecodeVertexLayout(vs, fetches) ||
          !BuildInputLayoutFromMicrocode(*vs, fetches, pending.stride,
                                         key.layout)) {
        if (g_immediate_unlayouted.fetch_add(1, std::memory_order_relaxed) < 6) {
          std::string why;
          if (!vs) {
            why = "no vertex shader bound";
          } else {
            VertexLayout f2;
            if (!DecodeVertexLayout(vs, f2)) {
              why = "no fetch records";
            } else {
              const u32 micro = MicrocodeAddress(*vs);
              why = fmt::format("micro{:08X} n{}", micro, f2.count);
              for (u32 i = 0; i < f2.count; ++i) {
                const FetchMicrocode d = DecodeFetch(micro, f2.fetches[i]);
                why += fmt::format(" {}{}[a{} p{}{} off{} strd{} fmt{}]",
                                   VertexUsageName(f2.fetches[i].usage),
                                   f2.fetches[i].usageIndex,
                                   f2.fetches[i].instructionAddress,
                                   f2.fetches[i].parentAddress,
                                   f2.fetches[i].miniFetch ? " mini" : "",
                                   d.offset, d.stride, d.format);
              }
            }
          }
          EOT_WARN("[immediate] skipped stride={} prim={}: {}", pending.stride,
                   pending.primitiveType, why);
        }
        Video::SetImmediateStream(0, 0, 0);
        return;
      }

      if (auto *pipeline = GetOrCreatePipeline(key, key.layout)) {
        DrawArgs args;
        args.primitiveType = pending.primitiveType;
        args.vertexCount = pending.vertexCount;
        IssueDraw(pending.device_va, key, pipeline, false, args);
        if (g_immediate_issued.fetch_add(1, std::memory_order_relaxed) < 4) {
          std::string lay;
          for (u32 e = 0; e < key.layout.count; ++e)
            lay += fmt::format(" {}{}@{}:{}",
                               VertexUsageName(key.layout.elements[e].usage),
                               key.layout.elements[e].usageIndex,
                               key.layout.elements[e].offset,
                               u32(key.layout.elements[e].format));
          EOT_INFO("[immediate] issued prim={} count={} stride={} vs{:08X}:{}",
                   pending.primitiveType, pending.vertexCount, pending.stride,
                   vs ? vs->objectVa : 0, lay);
        }
      }
    }
  }

  Video::SetImmediateStream(0, 0, 0);
}

void LogImmediateStats() {
  const u32 seen = g_immediate_seen.load();
  if (!seen)
    return;
  EOT_INFO("[immediate] {} BeginVertices allocations, {} issued as draws, {} "
           "skipped for microcode that does not fit the stride",
           seen, g_immediate_issued.load(), g_immediate_unlayouted.load());
}

void NoteFrameStartForDraws() {
  g_frame_clear_pending.store(true, std::memory_order_relaxed);
}

}

REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_HOOK_RAW(D3DDevice_DrawVertices) {
  const u32 device_va = ctx.r3.u32;
  eot::gpu::DrawArgs args;
  args.primitiveType = ctx.r4.u32;
  args.startVertex = ctx.r5.u32;
  args.vertexCount = ctx.r6.u32;
  __imp__D3DDevice_DrawVertices(ctx, base);
  eot::gpu::CountDraw(device_va, false, args);
}

REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  const u32 device_va = ctx.r3.u32;
  eot::gpu::DrawArgs args;
  args.primitiveType = ctx.r4.u32;
  args.baseVertexIndex = ctx.r5.u32;
  args.startIndex = ctx.r6.u32;
  args.indexCount = ctx.r7.u32;
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
  eot::gpu::CountDraw(device_va, true, args);
}

REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_HOOK_RAW(D3DDevice_BeginVertices) {
  const u32 device_va = ctx.r3.u32;
  const u32 primitive_type = ctx.r4.u32;
  const u32 vertex_count = ctx.r5.u32;
  const u32 stride = ctx.r6.u32;
  __imp__D3DDevice_BeginVertices(ctx, base);
  eot::gpu::NoteImmediateVertices(device_va, primitive_type, vertex_count,
                                  stride, ctx.r3.u32);
}
