#include <atomic>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device/device.h"
#include "gpu/device/host_resource_heap.h"
#include "gpu/device/native_texture_mirror.h"
#include "gpu/guest/d3d.h"
#include "gpu/guest/format.h"

namespace {

std::atomic<u32> g_mrt_reported[eot::gpu::kMaxRenderTargets];
std::atomic<u32> g_unresolved_rt{0};
std::atomic<u32> g_resolved_rt{0};
std::atomic<u32> g_unresolved_ds{0};
std::atomic<u32> g_resolved_ds{0};

void ReportRenderTarget(u32 index, u32 surface_va,
                        const eot::gpu::GuestTexture *resolved) {
  if (index > 0 && surface_va &&
      g_mrt_reported[index].fetch_add(1, std::memory_order_relaxed) == 0) {
    EOT_INFO("[rt] MRT in use: slot {} bound to non-null surface 0x{:08X} "
             "(only slot 0 reaches the framebuffer)",
             index, surface_va);
  }
  if (!surface_va)
    return;
  if (resolved) {
    if (g_resolved_rt.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_INFO("[rt] slot {} surface 0x{:08X} resolved: {}x{} fmt={}", index,
               surface_va, resolved->width, resolved->height,
               static_cast<u32>(resolved->format));
    }
  } else if (g_unresolved_rt.fetch_add(1, std::memory_order_relaxed) == 0) {
    EOT_INFO("[rt] slot {} surface 0x{:08X} unresolved: neither heap-owned nor a "
             "registered pool mirror",
             index, surface_va);
  }
}

void ReportDepthStencil(u32 surface_va,
                        const eot::gpu::GuestTexture *resolved) {
  if (!surface_va)
    return;
  if (resolved) {
    if (g_resolved_ds.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_INFO("[rt] depth surface 0x{:08X} resolved: {}x{} fmt={}", surface_va,
               resolved->width, resolved->height,
               static_cast<u32>(resolved->format));
    }
  } else if (g_unresolved_ds.fetch_add(1, std::memory_order_relaxed) == 0) {
    EOT_INFO("[rt] depth surface 0x{:08X} NOT owned by HostResourceHeap - needs "
             "pool registration",
             surface_va);
  }
}

}

REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
REX_HOOK_RAW(D3DDevice_SetRenderTarget) {
  const u32 device_va = ctx.r3.u32;
  const u32 index = ctx.r4.u32;
  const u32 surface_va = ctx.r5.u32;

  if (device_va && index < eot::gpu::kMaxRenderTargets) {
    eot::mem::store<u32>(
        device_va + eot::gpu::kDeviceRenderTargetShadow + index * 4,
        surface_va);
  }

  __imp__D3DDevice_SetRenderTarget(ctx, base);

  auto *surface = eot::gpu::ResolveGuestSurface(surface_va);
  ReportRenderTarget(index, surface_va, surface);
  eot::gpu::Video::SetRenderTarget(index, surface);
}

REX_EXTERN(__imp__D3DDevice_SetDepthStencilSurface);
REX_HOOK_RAW(D3DDevice_SetDepthStencilSurface) {
  const u32 device_va = ctx.r3.u32;
  const u32 surface_va = ctx.r4.u32;

  if (device_va) {
    eot::mem::store<u32>(device_va + eot::gpu::kDeviceDepthStencilShadow,
                         surface_va);
  }

  __imp__D3DDevice_SetDepthStencilSurface(ctx, base);

  auto *surface = eot::gpu::ResolveGuestSurface(surface_va);
  ReportDepthStencil(surface_va, surface);
  if (surface) {
    const bool is_depth =
        surface->type == eot::gpu::ResourceType::DepthStencil ||
        (surface->type == eot::gpu::ResourceType::Texture &&
         eot::gpu::IsDepthFormat(surface->format));
    if (!is_depth)
      surface = nullptr;
  }
  eot::gpu::Video::SetDepthStencil(surface);
}
