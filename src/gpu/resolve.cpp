#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/gpu_timing.h"
#include "gpu/gpu_profiling.h"

#include "gpu/draw.h"
#include "gpu/format.h"
#include "gpu/settings.h"
#include "gpu/surfaces.h"
#include "gpu/textures.h"
#include "gpu/trace.h"

namespace eot::gpu {

namespace xe = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;

namespace {

bool LocateAlias(const GuestTexture &d, const GuestTexture &t, i32 &tx, i32 &ty) {
  if (t.format != d.format || t.tiled != d.tiled)
    return false;
  const u32 pitch = ((d.fetch[0] >> 22) & 0x1FF) * 32;
  if (!pitch || pitch != ((t.fetch[0] >> 22) & 0x1FF) * 32)
    return false;
  const rex::graphics::FormatInfo *fi =
      rex::graphics::FormatInfo::Get(static_cast<u32>(d.format));
  if (!fi || fi->block_width != 1 || fi->block_height != 1)
    return false;
  const u32 bpb = fi->bytes_per_block();
  if (bpb < 4)
    return false;
  u32 bpb_log2 = 0;
  while ((1u << bpb_log2) < bpb)
    ++bpb_log2;
  const bool t_after = t.baseAddress >= d.baseAddress;
  const u32 delta = t_after ? t.baseAddress - d.baseAddress : d.baseAddress - t.baseAddress;
  i32 ox = 0, oy = 0;
  if (d.tiled) {
    const u32 tile_bytes = 32 * 32 * bpb;
    const u32 tiles_per_row = pitch / 32;
    if (!tiles_per_row || delta % tile_bytes)
      return false;
    const u32 tile = delta / tile_bytes;
    ox = static_cast<i32>((tile % tiles_per_row) * 32);
    oy = static_cast<i32>((tile / tiles_per_row) * 32);
    if (tu::GetTiledOffset2D(ox, oy, pitch, bpb_log2) != static_cast<i32>(delta))
      return false;
  } else {
    const u32 row_bytes = pitch * bpb;
    oy = static_cast<i32>(delta / row_bytes);
    ox = static_cast<i32>((delta % row_bytes) / bpb);
  }
  tx = t_after ? ox : -ox;
  ty = t_after ? oy : -oy;
  return true;
}

i32 Signed6(u32 v) {
  v &= 0x3F;
  return v & 0x20 ? static_cast<i32>(v) - 64 : static_cast<i32>(v);
}

u32 ResolveStoreSwizzle(u32 fetch3) {
  const u32 sw = (fetch3 >> 1) & 0xFFF;
  constexpr u32 kSwapRedBlue = 0x60A;
  return (sw & 7) == 2 ? (0x80000000u | kSwapRedBlue) : 0u;
}

void ClearSource(VideoState &s, GuestSurface &surf, u32 clear_color_va, float clear_z) {
  surf.redirectMirror.reset();
  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (!surf.isDepth && clear_color_va) {
    for (u32 i = 0; i < 4; ++i)
      rgba[i] = mem::f32at(clear_color_va + 4 * i);
  }
  auto clear_image = [&](HostTexture &image) {
    HostTexture *colors[4] = {surf.isDepth ? nullptr : &image, nullptr, nullptr, nullptr};
    plume::RenderFramebuffer *fb =
        GetFramebuffer(s, colors, surf.isDepth ? 0u : 1u, surf.isDepth ? &image : nullptr);
    if (!fb)
      return;
    TransitionLocked(s, image,
                     surf.isDepth ? plume::RenderTextureLayout::DEPTH_WRITE
                                  : plume::RenderTextureLayout::COLOR_WRITE);
    if (s.bound_framebuffer != fb)
      s.command_list->setFramebuffer(fb);
    s.bound_framebuffer = fb;
    s.bound_draw_targets_valid = false;
    if (surf.isDepth)
      s.command_list->clearDepthStencil(true, true, clear_z, 0, nullptr, 0);
    else
      s.command_list->clearColor(0, plume::RenderColor(rgba[0], rgba[1], rgba[2], rgba[3]), nullptr, 0);
    image.needsClear = false;
  };
  clear_image(surf.host);
  if (SurfaceHasSingle(surf))
    clear_image(surf.single);
  if (surf.isDepth)
    NoteSurfaceClearedDepth(surf, clear_z, 0, true, SurfaceHasSingle(surf));
  else
    NoteSurfaceClearedColor(surf, surf.host, rgba, true, true);
  surf.perfClears++;
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
  EOT_CPU_ZONE("ResolveGuest");
  PerfScope perf_scope(s.perf.resolve_ms);
  s.perf.resolves++;
  FlushPendingTransitions(s);
  GpuTimingMark(s, s.command_list, kGpuCatResolve);

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
  surf->perfResolves++;

  if (!depth_source && surf->content == GuestSurface::Content::Borrowed)
    SurfaceTakeBack(s, *surf);
  HostTexture *src_host = depth_source ? &surf->host : &SurfaceContentImage(*surf);
  GuestSurface *alias_src = nullptr;
  {
    PerfScope msaa_scope(s.perf.msaa_scan_ms);
    alias_src = FindMultisampleAliasSource(s, *surf);
  }
  if (alias_src) {
    src_host = depth_source ? &alias_src->host : SurfaceContentPeek(s, *alias_src);
    if (!src_host)
      return;
    u32 n;
    if (DiagShouldLog(0x7400 ^ src_va, &n) && n == 0)
      EOT_DEBUG("[resolve] {:#x}: {}x{} {}x-msaa alias -> sampling {}x{} 1x surface {:#x}", src_va,
               surf->width, surf->height, surf->msaaSamples, alias_src->width, alias_src->height,
               alias_src->va);
  }
  if (src_host->sampleCount > 1) {
    GuestSurface &src_surf = alias_src ? *alias_src : *surf;
    if (HostTexture *single = SurfaceColorSingle(s, src_surf))
      src_host = single;
  }

  GuestTexture *dest = nullptr;
  {
    EOT_CPU_ZONE("resolve destination mirror");
    PerfScope mirror_scope(s.perf.resolve_mirror_ms);
    dest = dest_texture_va ? GetGuestTexture(s, dest_texture_va, false) : nullptr;
    if (dest && !EnsureResolveMirror(s, *dest, depth_source, surf->scale))
      dest = nullptr;
  }

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

  const bool whole_src = x0 == 0 && y0 == 0 && rw == static_cast<i32>(surf->width) &&
                         rh == static_cast<i32>(surf->height);
  bool redirect_hit = false;
  if (depth_source && surf->redirectMirror) {
    const i32 hx = ScalePxBy(dx, surf->scale), hy = ScalePxBy(dy, surf->scale);
    if (dest == surf->redirectMirror.get() && whole_src && dest_level == 0 &&
        hx == surf->redirectX && hy == surf->redirectY) {
      redirect_hit = true;
      src_host = &surf->redirectMirror->host;
    } else if (!SurfaceRedirectEnd(s, *surf)) {
      return;
    }
  }
  const bool src_is_region = redirect_hit;
  const i32 src_origin_x = src_is_region ? surf->redirectX : 0;
  const i32 src_origin_y = src_is_region ? surf->redirectY : 0;

  if (dest && rw > 0 && rh > 0 && dest_level < dest->host.mipLevels) {
    const i32 copy_exp = Signed6(flags >> 26);
    const i32 tex_exp = Signed6(dest->fetch[3] >> 13);
    const i32 rt_exp = surf->colorExpBias;
    const i32 net_exp = rt_exp + copy_exp + tex_exp;
    const float scale = 1.0f;
    {
      u32 n;
      const bool diag_now =
          Settings::DiagFrame() > 0 && s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame());
      if ((DiagShouldLog(0x7100 ^ dest_texture_va ^ (dest_level << 8), &n) && n == 0) || diag_now) {
        EOT_DEBUG("[resolve] {} {:#x} -> tex {:#x} mip {} rect {},{}-{},{} at {},{} exp rt{} "
                  "copy{} tex{} => x{} ({} -> host fmt {}) fetch=[{:08x} {:08x} {:08x} {:08x}] "
                  "base {:#x} pitch {} {}x{} {}",
                  depth_source ? "depth" : "color", src_va, dest_texture_va, dest_level, x0, y0,
                  x1, y1, dx, dy, rt_exp, copy_exp, tex_exp, std::ldexp(1.0f, net_exp),
                  static_cast<u32>(dest->format), static_cast<u32>(dest->host.format),
                  dest->fetch[0], dest->fetch[1], dest->fetch[2], dest->fetch[3],
                  dest->baseAddress, ((dest->fetch[0] >> 22) & 0x1FF) * 32, dest->width,
                  dest->height, dest->tiled ? "tiled" : "linear");
      }
    }
    std::shared_ptr<GuestTexture> dest_ref;
    if (auto it = s.textures.find(dest_texture_va); it != s.textures.end())
      dest_ref = it->second;
    auto mark = [&](GuestTexture &target, u32 level) {
      target.resolveOwned = true;
      target.uploaded = true;
      target.uploadedUnlockSeq = ResourceUnlockSeq(target.va);
      target.resolvedMipMask |= 1u << level;
      target.lastUseFrame = s.guest_frames;
      if (target.lastResolvedFrame && target.lastSampledFrame < target.lastResolvedFrame) {
        s.perf.dead_resolves++;
        target.perfDeadResolves++;
      }
      target.lastResolvedFrame = s.guest_frames;
      target.perfResolves++;
    };
    static const bool copies = Settings::ResolveCopy();
    plume::RenderViewport last_resolve_vp;
    plume::RenderRect last_resolve_sc;
    bool resolve_dynamic_valid = false;
    auto blit = [&](GuestTexture &target, u32 level, i32 vx, i32 vy, i32 vw, i32 vh, i32 sx0,
                    i32 sy0) -> bool {
      const i32 mip_w = static_cast<i32>(std::max(1u, target.width >> level));
      const i32 mip_h = static_cast<i32>(std::max(1u, target.height >> level));
      const bool whole = sx0 == 0 && sy0 == 0 && vx == 0 && vy == 0 &&
                         vw == static_cast<i32>(surf->width) &&
                         vh == static_cast<i32>(surf->height) && vw == mip_w && vh == mip_h;
      const bool same_format = src_host->format == target.host.format;
      const bool reorder = !depth_source && ResolveStoreSwizzle(target.fetch[3]) != 0;
      const bool covers_image = whole && target.host.mipLevels == 1;
      const bool ms_src = src_host->sampleCount > 1;
      const GuestSurface &content_surf = alias_src ? *alias_src : *surf;
      const i32 rect_now[6] = {vx, vy, vw, vh, sx0, sy0};
      if (target.resolvedSurfaceUid == content_surf.uid &&
          target.resolvedSurfaceSerial == content_surf.serial &&
          target.resolvedOwnSerial == target.contentSerial && target.resolvedLevel == level &&
          std::memcmp(target.resolvedRect, rect_now, sizeof(rect_now)) == 0 &&
          !target.host.needsClear && !reorder == !target.storeSwapRB) {
        s.perf.resolve_noops++;
        mark(target, level);
        return true;
      }
      TextureReleaseBorrower(s, target);
      target.contentSerial++;
      target.resolvedSurfaceUid = content_surf.uid;
      target.resolvedSurfaceSerial = content_surf.serial;
      target.resolvedOwnSerial = target.contentSerial;
      target.resolvedLevel = level;
      std::memcpy(target.resolvedRect, rect_now, sizeof(rect_now));
      if (s.guest_frames - surf->regretResetFrame >= 128) {
        surf->regretResetFrame = s.guest_frames;
        surf->regretMask = 0;
      }
      if (surf->resolveOrdinalFrame != s.guest_frames) {
        surf->resolveOrdinalFrame = s.guest_frames;
        surf->resolveOrdinal = 0;
      }
      const u32 ordinal = surf->resolveOrdinal < 32 ? surf->resolveOrdinal : 31;
      const bool regretted = (surf->regretMask >> ordinal) & 1u;
      surf->resolveOrdinal++;
      const float k = surf->scale;
      const i32 hx0 = ScalePxBy(vx, k), hy0 = ScalePxBy(vy, k), hx1 = ScalePxBy(vx + vw, k),
                hy1 = ScalePxBy(vy + vh, k);
      if (hx1 <= hx0 || hy1 <= hy0)
        return true;
      const i32 sx0h = ScalePxBy(sx0, k) + src_origin_x, sy0h = ScalePxBy(sy0, k) + src_origin_y;
      const i32 mip_w_host = static_cast<i32>(std::max(1u, target.host.width >> level));
      const i32 mip_h_host = static_cast<i32>(std::max(1u, target.host.height >> level));
      const bool copy_fits = sx0h + (hx1 - hx0) <= static_cast<i32>(src_host->width) &&
                             sy0h + (hy1 - hy0) <= static_cast<i32>(src_host->height) &&
                             hx1 <= mip_w_host && hy1 <= mip_h_host;
      const bool whole_host = whole && src_host->width == static_cast<u32>(mip_w_host) &&
                              src_host->height == static_cast<u32>(mip_h_host);
      const bool own_image = src_host == &surf->single || src_host == &surf->host;
      if (Settings::ResolveTransfer() && !regretted && &target == dest && dest_ref && !depth_source &&
          !ms_src && !alias_src && own_image && same_format && scale == 1.0f && level == 0 &&
          whole_host && covers_image && target.host.viewFormat == plume::RenderFormat::UNKNOWN &&
          target.host.arraySize == 1 && target.host.depth == 1 && target.host.sampleCount == 1 &&
          target.host.desc.flags == src_host->desc.flags &&
          target.host.desc.dimension == src_host->desc.dimension &&
          target.host.desc.committed == src_host->desc.committed) {
        if (target.storeSwapRB != reorder) {
          target.storeSwapRB = reorder;
          target.bindingGeneration++;
        }
        target.host.needsClear = false;
        surf->handoffOrdinal = ordinal;
        SurfaceTransferToMirror(s, *surf, *src_host, target, dest_ref);
        src_host = &target.host;
        mark(target, level);
        return true;
      }
      if (copies && !ms_src && !alias_src && same_format && scale == 1.0f && copy_fits &&
          (!depth_source || whole_host) && (covers_image || !target.host.needsClear) &&
          (!reorder || covers_image)) {
        if (covers_image)
          target.host.needsClear = false;
        if (target.storeSwapRB != reorder) {
          target.storeSwapRB = reorder;
          target.bindingGeneration++;
        }
        auto *cmd = s.command_list;
        const HostTextureTransition transitions[] = {
            {src_host, plume::RenderTextureLayout::COPY_SOURCE},
            {&target.host, plume::RenderTextureLayout::COPY_DEST}};
        TransitionManyLocked(s, transitions, 2);
        const plume::RenderBox box(sx0h, sy0h, sx0h + (hx1 - hx0), sy0h + (hy1 - hy0));
        if (GpuTimingDiagActive(s))
          GpuTimingDiagMark(s, cmd,
                            std::format("resolve copy {:#x} {}x{} {}", dest_texture_va, hx1 - hx0,
                                        hy1 - hy0, depth_source ? "depth" : "colour"));
        cmd->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(target.host.texture.get(), level, 0),
            plume::RenderTextureCopyLocation::Subresource(src_host->texture.get(), 0, 0),
            static_cast<u32>(hx0), static_cast<u32>(hy0), 0, whole_host ? nullptr : &box);
        s.perf.resolve_copies++;
        mark(target, level);
        return true;
      }
      {
        u32 n;
        const u32 key = 0x7500 ^ (static_cast<u32>(src_host->format) << 8) ^
                        static_cast<u32>(target.host.format) ^ (reorder ? 0x40000 : 0) ^
                        (alias_src ? 0x80000 : 0) ^ (depth_source ? 0x100000 : 0) ^
                        (ms_src ? 0x200000 : 0);
        if (copies && DiagShouldLog(key, &n) && n == 0)
          EOT_DEBUG("[resolve] blit kept: host fmt {} -> {}{}{}{}{}", static_cast<u32>(src_host->format),
                   static_cast<u32>(target.host.format), reorder ? " reorder" : "",
                   alias_src ? " msaa-alias" : "", depth_source && !whole ? " depth-subrect" : "",
                   ms_src ? " multisampled-source" : "");
      }
      plume::RenderFramebuffer *fb = nullptr;
      {
        EOT_CPU_ZONE("resolve framebuffer");
        PerfScope fb_scope(s.perf.resolve_fb_ms);
        fb = GetMipFramebuffer(s, target, level);
      }
      if (!fb)
        return false;
      auto *cmd = s.command_list;
      PerfScope bind_scope(s.perf.resolve_bind_ms);
      const HostTextureTransition transitions[] = {
          {src_host, plume::RenderTextureLayout::SHADER_READ},
          {&target.host, depth_source ? plume::RenderTextureLayout::DEPTH_WRITE
                                      : plume::RenderTextureLayout::COLOR_WRITE}};
      TransitionManyLocked(s, transitions, 2);
      if (target.host.needsClear) {
        for (u32 m = 0; m < target.host.mipLevels; ++m) {
          plume::RenderFramebuffer *mfb = m == level ? fb : GetMipFramebuffer(s, target, m);
          if (!mfb)
            continue;
          if (s.bound_framebuffer != mfb) {
            cmd->setFramebuffer(mfb);
            s.bound_framebuffer = mfb;
          }
          s.bound_draw_targets_valid = false;
          if (target.host.isDepth)
            cmd->clearDepthStencil(true, true, 0.0f, 0, nullptr, 0);
          else
            cmd->clearColor(0, plume::RenderColor(0, 0, 0, 0), nullptr, 0);
        }
        target.host.needsClear = false;
      }
      GpuTimingMark(s, cmd, depth_source ? kGpuCatResolveDepth : kGpuCatResolve);
      if (GpuTimingDiagActive(s))
        GpuTimingDiagMark(s, cmd,
                          std::format("resolve blit {:#x} {}x{} {}{}", dest_texture_va, hx1 - hx0,
                                      hy1 - hy0, depth_source ? "depth" : "colour",
                                      ms_src ? " ms" : ""));
      if (target.storeSwapRB) {
        target.storeSwapRB = false;
        target.bindingGeneration++;
      }
      if (s.bound_framebuffer != fb) {
        cmd->setFramebuffer(fb);
        s.bound_framebuffer = fb;
      }
      s.bound_draw_targets_valid = false;
      const plume::RenderFormat color_fmt = target.host.viewFormat != plume::RenderFormat::UNKNOWN
                                                ? target.host.viewFormat
                                                : target.host.format;
      plume::RenderPipeline *pso =
          ms_src ? GetResolveMsaaPipeline(s, depth_source ? target.host.format : color_fmt,
                                          src_host->sampleCount, depth_source)
          : depth_source ? GetDepthCopyPipeline(s, target.host.format)
                         : GetBlitPipeline(s, color_fmt);
      if (!pso)
        return false;
      if (s.bound_pipeline != pso) {
        cmd->setPipeline(pso);
        s.bound_pipeline = pso;
      }
      plume::RenderViewport vp(static_cast<float>(hx0), static_cast<float>(hy0),
                               static_cast<float>(hx1 - hx0), static_cast<float>(hy1 - hy0), 0.0f,
                               1.0f);
      plume::RenderRect sc(hx0, hy0, hx1, hy1);
      if (!resolve_dynamic_valid ||
          std::memcmp(&last_resolve_vp, &vp, sizeof(last_resolve_vp)) != 0) {
        cmd->setViewports(&vp, 1);
        last_resolve_vp = vp;
      }
      if (!resolve_dynamic_valid ||
          std::memcmp(&last_resolve_sc, &sc, sizeof(last_resolve_sc)) != 0) {
        cmd->setScissors(&sc, 1);
        last_resolve_sc = sc;
      }
      resolve_dynamic_valid = true;
      CopyPushConstants pc;
      pc.resourceDescriptorIndex = BindTextureSRVLocked(s, *src_host);
      pc.resourceDescriptorIndex2 = depth_source ? 0u : ResolveStoreSwizzle(target.fetch[3]);
      pc.param0 = scale;
      pc.param1 = 0.0f;
      const GuestSurface &src_surf = alias_src ? *alias_src : *surf;
      if (src_is_region) {
        pc.rect[0] = static_cast<float>(sx0h) / static_cast<float>(src_host->width);
        pc.rect[1] = static_cast<float>(sy0h) / static_cast<float>(src_host->height);
        pc.rect[2] = static_cast<float>(sx0h + (hx1 - hx0)) / static_cast<float>(src_host->width);
        pc.rect[3] = static_cast<float>(sy0h + (hy1 - hy0)) / static_cast<float>(src_host->height);
      } else {
        const float rx = static_cast<float>(src_surf.width) / static_cast<float>(surf->width);
        const float ry = static_cast<float>(src_surf.height) / static_cast<float>(surf->height);
        pc.rect[0] = static_cast<float>(sx0) * rx / static_cast<float>(src_surf.allocWidth);
        pc.rect[1] = static_cast<float>(sy0) * ry / static_cast<float>(src_surf.allocHeight);
        pc.rect[2] = static_cast<float>(sx0 + vw) * rx / static_cast<float>(src_surf.allocWidth);
        pc.rect[3] = static_cast<float>(sy0 + vh) * ry / static_cast<float>(src_surf.allocHeight);
      }
      bind_scope.stop();
      cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                    kCopyPushConstantByteOffset, sizeof(pc));
      {
        EOT_GPU_ZONE("resolve blit");
        cmd->drawInstanced(3, 1, 0, 0);
      }
      mark(target, level);
      return true;
    };

    const u32 mip_w = std::max(1u, dest->width >> dest_level);
    const u32 mip_h = std::max(1u, dest->height >> dest_level);
    const i32 vx = std::clamp(dx, 0, static_cast<i32>(mip_w));
    const i32 vy = std::clamp(dy, 0, static_cast<i32>(mip_h));
    const i32 vw = std::min(rw, static_cast<i32>(mip_w) - vx);
    const i32 vh = std::min(rh, static_cast<i32>(mip_h) - vy);
    if (vw > 0 && vh > 0 && redirect_hit) {
      dest->contentSerial++;
      dest->resolvedSurfaceUid = surf->uid;
      dest->resolvedSurfaceSerial = surf->serial;
      dest->resolvedOwnSerial = dest->contentSerial;
      dest->resolvedLevel = dest_level;
      const i32 rect_now[6] = {vx, vy, vw, vh, x0, y0};
      std::memcpy(dest->resolvedRect, rect_now, sizeof(rect_now));
      dest->host.needsClear = false;
      s.perf.resolve_noops++;
      mark(*dest, dest_level);
    } else if (vw > 0 && vh > 0) {
      blit(*dest, dest_level, vx, vy, vw, vh, x0, y0);
    }
    if (depth_source && whole_src && dest_level == 0 && vw == rw && vh == rh &&
        surf->host.sampleCount == 1 && dest_ref && dest->host.isDepth && dest->host.renderable &&
        dest->host.sampleCount == 1 && dest->host.mipLevels == 1 && dest->host.arraySize == 1 &&
        dest->host.format == surf->host.format && surf->redirectPassFrame == s.guest_frames &&
        surf->redirectPasses >= 1 && surf->redirectPasses <= GuestSurface::kRedirectPasses &&
        (dest->host.width > surf->host.width || dest->host.height > surf->host.height)) {
      const i32 hx = ScalePxBy(vx, surf->scale), hy = ScalePxBy(vy, surf->scale);
      if (hx >= 0 && hy >= 0 && hx + surf->host.width <= dest->host.width &&
          hy + surf->host.height <= dest->host.height) {
        GuestSurface::RedirectPrediction &p = surf->redirectPredictions[surf->redirectPasses - 1];
        p.mirror = dest_ref;
        p.texture = dest->host.texture.get();
        p.x = hx;
        p.y = hy;
      }
    }

    if (dest_level == 0 && vw > 0 && vh > 0) {
      s.perf.alias_scanned += static_cast<u32>(s.textures.size());
      PerfScope alias_scope(s.perf.alias_scan_ms);
      const u64 visit_token = ++s.resolve_alias_token;
      for (auto &[alias_va, alias] : s.textures) {
        GuestTexture *t = alias.get();
        if (!t || t == dest || !t->host.texture || !t->host.renderable ||
            t->host.isDepth != depth_source)
          continue;
        if (t->uploaded && !t->resolveOwned)
          continue;
        if (t->aliasVisitToken == visit_token)
          continue;
        t->aliasVisitToken = visit_token;
        i32 tx, ty;
        if (!LocateAlias(*dest, *t, tx, ty))
          continue;
        const i32 ax0 = vx - tx, ay0 = vy - ty;
        const i32 cx0 = std::max(ax0, 0), cy0 = std::max(ay0, 0);
        const i32 cx1 = std::min(ax0 + vw, static_cast<i32>(t->width));
        const i32 cy1 = std::min(ay0 + vh, static_cast<i32>(t->height));
        if (cx1 <= cx0 || cy1 <= cy0)
          continue;
        {
          u32 n;
          if (DiagShouldLog(0x7300 ^ alias_va ^ dest_texture_va, &n) && n == 0)
            EOT_DEBUG("[resolve] {:#x} also lands in {:#x} ({}x{} base {:#x}) at {},{} ({}x{})",
                     dest_texture_va, alias_va, t->width, t->height, t->baseAddress, cx0, cy0,
                     cx1 - cx0, cy1 - cy0);
        }
        blit(*t, 0, cx0, cy0, cx1 - cx0, cy1 - cy0, x0 + (cx0 - ax0), y0 + (cy0 - ay0));
      }
    }
    s.bound_pipeline = nullptr;
    s.bound_framebuffer = nullptr;
  } else if (!dest) {
    u32 n;
    if (DiagShouldLog(0x7002 ^ dest_texture_va, &n))
      EOT_WARN("[resolve] destination {:#x} has no usable host mirror", dest_texture_va);
  }

  if (Settings::DiagDump() && Settings::DiagFrame() > 0 &&
      s.guest_frames + 1 == static_cast<u64>(Settings::DiagFrame())) {
    static u32 k = 0;
    const std::string path = std::format("logs/f{}_r{}_{}_{:x}.ppm", s.guest_frames + 1, k++,
                                         depth_source ? "depth" : "color", src_va);
    if (surf->host.sampleCount == 1)
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
