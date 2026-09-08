#include "gpu/patches/present_effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/device.h"
#include "gpu/gpu_timing.h"
#include "gpu/settings.h"
#include "gpu/surfaces.h"

#if defined(EOT_D3D12)
#include "shaders/cas_ps.hlsl.dxil.h"
#include "shaders/fxaa_high_ps.hlsl.dxil.h"
#include "shaders/fxaa_ps.hlsl.dxil.h"
#else
#include "shaders/cas_ps.hlsl.spirv.h"
#include "shaders/fxaa_high_ps.hlsl.spirv.h"
#include "shaders/fxaa_ps.hlsl.spirv.h"
#endif

REXCVAR_DEFINE_STRING(eot_aa, "fxaa", "EdgeOfTime/Graphics",
                      "Anti-aliasing applied to the finished frame at present: off, fxaa "
                      "(NVIDIA FXAA 3.11, quality preset 12) or fxaa_high (preset 39: the "
                      "longest edge search, fewest jaggies, most cost).");
REXCVAR_DEFINE_DOUBLE(eot_sharpen, 0.5, "EdgeOfTime/Graphics",
                      "AMD FidelityFX contrast-adaptive sharpening at present: 0 = off, up to "
                      "1 = strongest. Restores the crispness the display scaling takes.")
    .range(0.0, 1.0);
REXCVAR_DEFINE_STRING(eot_upscale, "bicubic", "EdgeOfTime/Graphics",
                      "Filter used when the window is larger than the internal render: "
                      "bilinear; bicubic (Catmull-Rom through five bilinear taps, as sharp "
                      "as Lanczos-2 at a third of its cost); or lanczos (4x4 Lanczos-2, "
                      "sixteen taps). A window smaller than the render by a whole factor is "
                      "always box-filtered, which is how supersampling resolves.");

namespace eot::gpu {

namespace {

constexpr plume::RenderFormat kFormat = plume::RenderFormat::R8G8B8A8_UNORM;

struct Effects {
  HostTexture ping, pong;
  std::unique_ptr<plume::RenderShader> fxaa_ps, fxaa_high_ps, cas_ps;
  std::unique_ptr<plume::RenderPipeline> fxaa, fxaa_high, cas;
  bool shaders_failed = false;
};

Effects &effects() {
  static Effects e;
  return e;
}

bool EnsureTarget(VideoState &s, HostTexture &host, u32 w, u32 h) {
  if (host.texture && host.width == w && host.height == h)
    return true;
  if (host.texture)
    ParkHostTexture(s, host);
  host = HostTexture{};
  host.format = kFormat;
  host.width = w;
  host.height = h;
  host.mipLevels = 1;
  host.renderable = true;
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = w;
  desc.height = h;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = kFormat;
  desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  desc.committed = Settings::CommittedTextures();
  plume::RenderClearValue clear =
      plume::RenderClearValue::Color(plume::RenderColor(0, 0, 0, 1), kFormat);
  desc.optimizedClearValue = &clear;
  host.texture = CreateHostTexture(s.device.get(), desc, "present-fx");
  host.desc = desc;
  host.desc.optimizedClearValue = nullptr;
  host.layout = plume::RenderTextureLayout::UNKNOWN;
  return host.texture != nullptr;
}

plume::RenderPipeline *MakePipeline(VideoState &s, plume::RenderShader *ps, const char *tag) {
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = s.copy_vs.get();
  desc.pixelShader = ps;
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = kFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  auto pso = CreateHostGraphicsPipeline(s.device.get(), desc, tag);
  return pso ? pso.release() : nullptr;
}

bool EnsurePipelines(VideoState &s) {
  auto &e = effects();
  if (e.fxaa && e.fxaa_high && e.cas)
    return true;
  if (e.shaders_failed)
    return false;
  e.fxaa_ps = s.device->createShader(EOT_SHADER_BLOB(fxaa_ps), "main", kHostShaderFormat);
  e.fxaa_high_ps =
      s.device->createShader(EOT_SHADER_BLOB(fxaa_high_ps), "main", kHostShaderFormat);
  e.cas_ps = s.device->createShader(EOT_SHADER_BLOB(cas_ps), "main", kHostShaderFormat);
  if (e.fxaa_ps)
    e.fxaa.reset(MakePipeline(s, e.fxaa_ps.get(), "present-fxaa"));
  if (e.fxaa_high_ps)
    e.fxaa_high.reset(MakePipeline(s, e.fxaa_high_ps.get(), "present-fxaa-high"));
  if (e.cas_ps)
    e.cas.reset(MakePipeline(s, e.cas_ps.get(), "present-cas"));
  if (!e.fxaa || !e.fxaa_high || !e.cas) {
    e.shaders_failed = true;
    EOT_ERROR("[present-fx] the FXAA / CAS pipelines failed to build; the options are off");
    return false;
  }
  return true;
}

u32 RunPass(VideoState &s, plume::RenderPipeline *pso, u32 src, HostTexture &dst,
            const float extra[4]) {
  auto *cmd = s.command_list;
  TransitionLocked(s, dst, plume::RenderTextureLayout::COLOR_WRITE);
  HostTexture *colors[4] = {&dst, nullptr, nullptr, nullptr};
  plume::RenderFramebuffer *fb = GetFramebuffer(s, colors, 1, nullptr);
  if (!fb)
    return src;
  cmd->setFramebuffer(fb);
  s.bound_framebuffer = fb;
  const plume::RenderViewport vp(0.0f, 0.0f, static_cast<float>(dst.width),
                                 static_cast<float>(dst.height), 0.0f, 1.0f);
  const plume::RenderRect sc(0, 0, static_cast<i32>(dst.width), static_cast<i32>(dst.height));
  cmd->setViewports(&vp, 1);
  cmd->setScissors(&sc, 1);
  cmd->setPipeline(pso);
  s.bound_pipeline = pso;
  CopyPushConstants pc;
  pc.resourceDescriptorIndex = src;
  pc.param0 = 1.0f;
  pc.param1 = 1.0f;
  std::memcpy(pc.extra, extra, sizeof(pc.extra));
  cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc, kCopyPushConstantByteOffset,
                                sizeof(pc));
  cmd->drawInstanced(3, 1, 0, 0);
  TransitionLocked(s, dst, plume::RenderTextureLayout::SHADER_READ);
  const u32 index = BindTextureSRVLocked(s, dst);
  return index != kInvalidDescriptorIndex ? index : src;
}

}

u32 ApplyPresentEffects(VideoState &s, HostTexture &front, u32 front_srv) {
  const std::string aa = REXCVAR_GET(eot_aa);
  const bool fxaa = aa == "fxaa" || aa == "fxaa_high";
  const float sharpen = static_cast<float>(std::clamp(REXCVAR_GET(eot_sharpen), 0.0, 1.0));
  if ((!fxaa && sharpen <= 0.0f) || front_srv == kInvalidDescriptorIndex || !front.texture)
    return front_srv;
  if (!EnsurePipelines(s))
    return front_srv;
  GpuTimingMark(s, s.command_list, kGpuCatPresentFx);
  auto &e = effects();
  if (!EnsureTarget(s, e.ping, front.width, front.height) ||
      !EnsureTarget(s, e.pong, front.width, front.height))
    return front_srv;
  u32 src = front_srv;
  HostTexture *next = &e.ping;
  if (fxaa) {
    const bool high = aa == "fxaa_high";
    const float extra[4] = {high ? 1.0f : 0.75f, high ? 0.063f : 0.166f,
                            high ? 0.0312f : 0.0833f, 0.0f};
    src = RunPass(s, high ? e.fxaa_high.get() : e.fxaa.get(), src, *next, extra);
    next = next == &e.ping ? &e.pong : &e.ping;
  }
  if (sharpen > 0.0f) {
    const float extra[4] = {sharpen, 0.0f, 0.0f, 0.0f};
    src = RunPass(s, e.cas.get(), src, *next, extra);
  }
  return src;
}

void SelectPresentBlitMode(u32 src_w, u32 src_h, float dst_w, float dst_h, float extra[4]) {
  extra[0] = extra[1] = extra[2] = extra[3] = 0.0f;
  if (!src_w || !src_h || dst_w <= 0.0f || dst_h <= 0.0f)
    return;
  const float rx = static_cast<float>(src_w) / dst_w;
  const float ry = static_cast<float>(src_h) / dst_h;
  if (rx >= 1.9f && ry >= 1.9f) {
    const float n = std::round(std::min(rx, ry));
    if (std::fabs(rx - n) < 0.06f && std::fabs(ry - n) < 0.06f) {
      extra[0] = 1.0f;
      extra[1] = n;
      return;
    }
  }
  if (rx < 0.98f && ry < 0.98f) {
    const std::string filter = REXCVAR_GET(eot_upscale);
    if (filter == "lanczos")
      extra[0] = 2.0f;
    else if (filter == "bicubic")
      extra[0] = 3.0f;
  }
}

}
