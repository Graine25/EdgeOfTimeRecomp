#include <cmath>
#include <format>
#include <mutex>
#include <string>

#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/format.h"
#include "gpu/settings.h"
#include "gpu/surfaces.h"
#include "gpu/textures.h"
#include "gpu/trace.h"

namespace eot::gpu {

namespace xe = rex::graphics::xenos;

namespace {

i32 Signed6(u32 v) {
  v &= 0x3F;
  return v & 0x20 ? static_cast<i32>(v) - 64 : static_cast<i32>(v);
}

void ClearSource(VideoState &s, GuestSurface &surf, u32 clear_color_va, float clear_z) {
  HostTexture *colors[4] = {};
  HostTexture *depth = nullptr;
  if (surf.isDepth) {
    depth = &surf.host;
    TransitionLocked(s, surf.host, plume::RenderTextureLayout::DEPTH_WRITE);
  } else {
    colors[0] = &surf.host;
    TransitionLocked(s, surf.host, plume::RenderTextureLayout::COLOR_WRITE);
  }
  plume::RenderFramebuffer *fb = GetFramebuffer(s, colors, surf.isDepth ? 0 : 1, depth);
  if (!fb)
    return;
  s.command_list->setFramebuffer(fb);
  s.bound_framebuffer = fb;
  if (surf.isDepth) {
    s.command_list->clearDepthStencil(true, true, clear_z, 0, nullptr, 0);
  } else {
    plume::RenderColor c(0, 0, 0, 0);
    if (clear_color_va) {
      c = plume::RenderColor(mem::f32at(clear_color_va), mem::f32at(clear_color_va + 4),
                             mem::f32at(clear_color_va + 8), mem::f32at(clear_color_va + 12));
    }
    s.command_list->clearColor(0, c, nullptr, 0);
  }
}

}

void ResolveGuest(u32 device_va, u32 flags, u32 src_rect_va, u32 dest_texture_va,
                  u32 dest_point_va, u32 dest_level, u32 clear_color_va, float clear_z) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.ready)
    return;
  DeviceView dev = Device(device_va);
  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  const u32 source = flags & 7;
  const bool depth_source = source == 4;
  const u32 src_va = depth_source ? dev.U32(dev::kDepthSurface)
                                  : dev.U32(dev::kRenderTarget0 + 4 * (source & 3));
  GuestSurface *surf = src_va ? GetGuestSurface(s, src_va) : nullptr;
  if (!surf || !surf->host.valid()) {
    u32 n;
    if (DiagShouldLog(0x7001, &n))
      EOT_WARN("[resolve] source {} has no bound surface (flags {:#x})", source, flags);
    return;
  }

  GuestTexture *dest = dest_texture_va ? GetGuestTexture(s, dest_texture_va) : nullptr;
  if (dest && !EnsureResolveMirror(s, *dest, depth_source))
    dest = nullptr;

  i32 x0 = 0, y0 = 0, x1 = static_cast<i32>(surf->width), y1 = static_cast<i32>(surf->height);
  if (src_rect_va) {
    x0 = mem::load<i32>(src_rect_va);
    y0 = mem::load<i32>(src_rect_va + 4);
    x1 = mem::load<i32>(src_rect_va + 8);
    y1 = mem::load<i32>(src_rect_va + 12);
  }
  i32 dx = 0, dy = 0;
  if (dest_point_va) {
    dx = mem::load<i32>(dest_point_va);
    dy = mem::load<i32>(dest_point_va + 4);
  }
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  x1 = std::min(static_cast<i32>(surf->width), x1);
  y1 = std::min(static_cast<i32>(surf->height), y1);
  const i32 rw = x1 - x0, rh = y1 - y0;

  if (dest && rw > 0 && rh > 0 && dest_level < dest->host.mipLevels) {
    const i32 copy_exp = Signed6(flags >> 26);
    const i32 tex_exp = Signed6(dest->fetch[3] >> 13);
    const i32 rt_exp = surf->colorExpBias;
    const i32 net_exp = rt_exp + copy_exp + tex_exp;
    const float scale = 1.0f;
    {
      u32 n;
      if (DiagShouldLog(0x7100 ^ dest_texture_va ^ (dest_level << 8), &n) && n == 0) {
        EOT_INFO("[resolve] {} {:#x} -> tex {:#x} mip {} rect {},{}-{},{} at {},{} exp rt{} "
                  "copy{} tex{} => x{} ({} -> host fmt {}) fetch=[{:08x} {:08x} {:08x} {:08x}]",
                  depth_source ? "depth" : "color", src_va, dest_texture_va, dest_level, x0, y0,
                  x1, y1, dx, dy, rt_exp, copy_exp, tex_exp, std::ldexp(1.0f, net_exp),
                  static_cast<u32>(dest->format), static_cast<u32>(dest->host.format),
                  dest->fetch[0], dest->fetch[1], dest->fetch[2], dest->fetch[3]);
      }
    }
    const u32 mip_w = std::max(1u, dest->host.width >> dest_level);
    const u32 mip_h = std::max(1u, dest->host.height >> dest_level);
    plume::RenderFramebuffer *fb = GetMipFramebuffer(s, *dest, dest_level);
    if (fb) {
      auto *cmd = s.command_list;
      TransitionLocked(s, surf->host, plume::RenderTextureLayout::SHADER_READ);
      TransitionLocked(s, dest->host,
                       depth_source ? plume::RenderTextureLayout::DEPTH_WRITE
                                    : plume::RenderTextureLayout::COLOR_WRITE);
      cmd->setFramebuffer(fb);
      s.bound_framebuffer = fb;
      plume::RenderPipeline *pso =
          depth_source ? GetDepthCopyPipeline(s, dest->host.format)
                       : GetBlitPipeline(s, dest->host.viewFormat != plume::RenderFormat::UNKNOWN
                                                ? dest->host.viewFormat
                                                : dest->host.format);
      if (pso) {
        cmd->setPipeline(pso);
        s.bound_pipeline = pso;
        const i32 vx = std::clamp(dx, 0, static_cast<i32>(mip_w));
        const i32 vy = std::clamp(dy, 0, static_cast<i32>(mip_h));
        const i32 vw = std::min(rw, static_cast<i32>(mip_w) - vx);
        const i32 vh = std::min(rh, static_cast<i32>(mip_h) - vy);
        plume::RenderViewport vp(static_cast<float>(vx), static_cast<float>(vy),
                                 static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f);
        plume::RenderRect sc(vx, vy, vx + vw, vy + vh);
        cmd->setViewports(&vp, 1);
        cmd->setScissors(&sc, 1);
        CopyPushConstants pc;
        pc.resourceDescriptorIndex = BindTextureSRVLocked(s, surf->host);
        pc.param0 = scale;
        pc.param1 = 0.0f;
        pc.rect[0] = static_cast<float>(x0) / surf->width;
        pc.rect[1] = static_cast<float>(y0) / surf->height;
        pc.rect[2] = static_cast<float>(x1) / surf->width;
        pc.rect[3] = static_cast<float>(y1) / surf->height;
        cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                      kCopyPushConstantByteOffset, sizeof(pc));
        cmd->drawInstanced(3, 1, 0, 0);
        dest->resolveOwned = true;
        dest->uploaded = true;
        dest->uploadedUnlockSeq = ResourceUnlockSeq(dest->va);
        dest->resolvedMipMask |= 1u << dest_level;
        dest->lastUseFrame = s.guest_frames;
      }
      s.bound_pipeline = nullptr;
      s.bound_framebuffer = nullptr;
    }
  } else if (!dest) {
    u32 n;
    if (DiagShouldLog(0x7002 ^ dest_texture_va, &n))
      EOT_WARN("[resolve] destination {:#x} has no usable host mirror", dest_texture_va);
  }

  if (Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) {
    static u32 k = 0;
    const std::string path = std::format("logs/f{}_r{}_{}_{:x}.ppm", s.guest_frames + 1, k++,
                                         depth_source ? "depth" : "color", src_va);
    DumpHostTextureLocked(s, surf->host, path.c_str(), depth_source ? 1.0f : 1.0f);
    if (!s.command_list_open)
      return;
  }
  if (flags & 0x100)
    ClearSource(s, *surf, clear_color_va, clear_z);
  if ((flags & 0x200) && depth_source)
    ClearSource(s, *surf, 0, clear_z);
  else if (flags & 0x200) {
    const u32 ds_va = dev.U32(dev::kDepthSurface);
    if (GuestSurface *ds = ds_va ? GetGuestSurface(s, ds_va) : nullptr)
      ClearSource(s, *ds, 0, clear_z);
  }
  s.bound_framebuffer = nullptr;
  DrainHostDebugMessages(s, "resolve");
}

}
