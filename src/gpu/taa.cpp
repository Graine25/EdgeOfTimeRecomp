#include "gpu/taa.h"

#include <cmath>
#include <cstring>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/gpu_profiling.h"
#include "gpu/gpu_timing.h"
#include "gpu/resources.h"
#include "gpu/settings.h"
#include "gpu/surfaces.h"
#include "gpu/textures.h"

#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#include "shaders/taa_ps.hlsl.dxil.h"
#else
#include "shaders/taa_ps.hlsl.spirv.h"
#endif

namespace eot::gpu::taa {

namespace {

constexpr u64 kPostFXMainOpaqueP = 0xfbc5fc4812582bb7ull;
constexpr u64 kPostFXDepthPassP = 0xf8ec65264a3884d8ull;
constexpr u64 kPostFXMainP = 0xcc7d0e94beb5c24aull;
constexpr u64 kPostFXVelocityBlurP = 0x23f8bff9bf8e2021ull;
constexpr u64 kPostFXBlendP = 0x36e64a4219844002ull;

constexpr u32 kJitterPhases = 8;

float Halton(u32 index, u32 base) {
  float f = 1.0f, r = 0.0f;
  for (u32 i = index + 1; i; i /= base) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(i % base);
  }
  return r;
}

struct alignas(16) Constants {
  float reproject[16];
  float jitter[4];
  float params[4];
  u32 indices[4];
};
static_assert(sizeof(Constants) == 112);

struct State {
  std::unique_ptr<plume::RenderShader> ps;
  std::unique_ptr<plume::RenderPipeline> pso;
  plume::RenderFormat psoFormat = plume::RenderFormat::UNKNOWN;
  HostTexture history[2];
  u32 write = 0;
  bool historyValid = false;
  float prevViewProjection[16] = {};
  bool prevValid = false;
  u64 lastFrame = ~0ull;
  u32 lastWidth = 0, lastHeight = 0;
  bool shaderFailed = false;
};

State &state() {
  static State s;
  return s;
}

bool Invert(const float m[16], float out[16]) {
  double a[16];
  for (int i = 0; i < 16; ++i)
    a[i] = m[i];
  double inv[16];
  inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
           a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
           a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
           a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
            a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
           a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
           a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
           a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
            a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
           a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
           a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
            a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
            a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
           a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
           a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
            a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
            a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
  const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  if (!std::isfinite(det) || std::fabs(det) < 1e-30)
    return false;
  const double rd = 1.0 / det;
  for (int i = 0; i < 16; ++i)
    out[i] = static_cast<float>(inv[i] * rd);
  return true;
}

void Multiply(const float a[16], const float b[16], float out[16]) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                       a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

bool IsFullFrameSceneMirror(const GuestTexture *t) {
  return t && t->resolveOwned && !t->host.isDepth && t->host.valid() &&
         t->width == kGuestRenderWidth && t->height == kGuestRenderHeight &&
         t->host.format == plume::RenderFormat::R16G16B16A16_FLOAT && t->host.sampleCount == 1;
}

bool IsFullFrameDepthMirror(const GuestTexture *t) {
  return t && t->host.isDepth && t->host.valid() && t->width == kGuestRenderWidth &&
         t->height == kGuestRenderHeight && t->host.sampleCount == 1;
}

bool EnsureShader(VideoState &s, State &st, plume::RenderFormat format) {
  if (st.shaderFailed)
    return false;
  if (!st.ps) {
    st.ps = s.device->createShader(EOT_SHADER_BLOB(taa_ps), "main", kHostShaderFormat);
    if (!st.ps) {
      st.shaderFailed = true;
      EOT_ERROR("[taa] createShader failed; temporal anti-aliasing off");
      return false;
    }
  }
  if (st.pso && st.psoFormat == format)
    return true;
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = s.copy_vs.get();
  desc.pixelShader = st.ps.get();
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  st.pso = CreateHostGraphicsPipeline(s.device.get(), desc, "taa");
  st.psoFormat = format;
  if (!st.pso) {
    st.shaderFailed = true;
    EOT_ERROR("[taa] pipeline creation failed; temporal anti-aliasing off");
    return false;
  }
  return true;
}

bool EnsureHistory(VideoState &s, State &st, const HostTexture &scene) {
  if (st.lastWidth == scene.width && st.lastHeight == scene.height && st.history[0].valid() &&
      st.history[1].valid())
    return true;
  for (HostTexture &h : st.history)
    if (h.valid())
      ParkHostTexture(s, h);
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      scene.width, scene.height, 1, scene.format, plume::RenderTextureFlag::RENDER_TARGET);
  for (HostTexture &h : st.history) {
    h.format = desc.format;
    h.viewFormat = plume::RenderFormat::UNKNOWN;
    h.width = desc.width;
    h.height = desc.height;
    h.depth = 1;
    h.mipLevels = 1;
    h.arraySize = 1;
    h.sampleCount = 1;
    h.isDepth = false;
    h.renderable = true;
    if (!CreateOrRecycleHostTexture(s, h, desc, "taa-history")) {
      EOT_ERROR("[taa] history image {}x{} failed; temporal anti-aliasing off", desc.width,
                desc.height);
      return false;
    }
  }
  st.lastWidth = scene.width;
  st.lastHeight = scene.height;
  st.historyValid = false;
  st.prevValid = false;
  EOT_INFO("[taa] history {}x{} ({} phases, feedback {:.2f})", desc.width, desc.height,
           kJitterPhases, Settings::TaaFeedback());
  return true;
}

void BindConstants(VideoState &s, const UploadAlloc &alloc) {
  auto *cmd = s.command_list;
#if defined(EOT_D3D12)
  if (alloc.gpuVa && s.root_cbv_index[0] != ~0u) {
    static_cast<plume::D3D12CommandList *>(cmd)->d3d->SetGraphicsRootConstantBufferView(
        s.root_cbv_index[0], alloc.gpuVa);
  } else
#endif
  {
    cmd->setGraphicsRootDescriptor(plume::RenderBufferReference(alloc.buffer, alloc.offset), 0);
  }
  s.bound_root_buffer[0] = nullptr;
  s.bound_root_offset[0] = 0;
}

}

bool IsSceneConsumer(u64 ps_hash) {
  return ps_hash == kPostFXMainOpaqueP || ps_hash == kPostFXDepthPassP ||
         ps_hash == kPostFXMainP || ps_hash == kPostFXVelocityBlurP || ps_hash == kPostFXBlendP;
}

u32 JitterIndex(const VideoState &s) {
  if (!Settings::Taa())
    return 0;
  return 1u + static_cast<u32>(s.guest_frames % kJitterPhases);
}

void FrameJitter(const VideoState &s, float *jx, float *jy) {
  *jx = 0.0f;
  *jy = 0.0f;
  if (!Settings::Taa())
    return;
  const u32 phase = static_cast<u32>(s.guest_frames % kJitterPhases);
  *jx = Halton(phase, 2) - 0.5f;
  *jy = Halton(phase, 3) - 0.5f;
}

void Reset(VideoState &) {
  State &st = state();
  st.historyValid = false;
  st.prevValid = false;
}

void Shutdown(VideoState &s) {
  State &st = state();
  for (HostTexture &h : st.history)
    if (h.valid())
      ParkHostTexture(s, h);
  st.pso.reset();
  st.ps.reset();
  st.psoFormat = plume::RenderFormat::UNKNOWN;
  st.lastWidth = st.lastHeight = 0;
  st.historyValid = false;
  st.prevValid = false;
}

void BeforeSceneConsumerDraw(VideoState &s, GuestTexture *const bound[16], const float *camera_vp) {
  State &st = state();
  if (!Settings::Taa() || !camera_vp) {
    st.historyValid = false;
    st.prevValid = false;
    return;
  }
  if (st.lastFrame == s.guest_frames)
    return;
  GuestTexture *scene = bound[0];
  if (!IsFullFrameSceneMirror(scene))
    return;
  GuestTexture *depth = nullptr;
  for (u32 i = 1; i < 16 && !depth; ++i)
    if (IsFullFrameDepthMirror(bound[i]))
      depth = bound[i];
  if (!s.command_list_open || !EnsureShader(s, st, scene->host.format) ||
      !EnsureHistory(s, st, scene->host))
    return;

  Constants c{};
  float inv[16];
  const bool reproject_ok = st.prevValid && Invert(camera_vp, inv);
  if (reproject_ok)
    Multiply(inv, st.prevViewProjection, c.reproject);
  float jx, jy;
  FrameJitter(s, &jx, &jy);
  const float w = static_cast<float>(scene->host.width), h = static_cast<float>(scene->host.height);
  c.jitter[0] = jx / w;
  c.jitter[1] = jy / h;
  c.jitter[2] = 1.0f / w;
  c.jitter[3] = 1.0f / h;
  c.params[0] = static_cast<float>(Settings::TaaFeedback());
  c.params[1] = st.historyValid && reproject_ok && depth ? 1.0f : 0.0f;
  c.params[2] = w;
  c.params[3] = h;

  const u32 read = st.write ^ 1u;
  HostTexture &history_in = st.history[read];
  HostTexture &history_out = st.history[st.write];

  FlushPendingTransitions(s);
  const HostTextureTransition transitions[] = {
      {&scene->host, plume::RenderTextureLayout::SHADER_READ},
      {&history_in, plume::RenderTextureLayout::SHADER_READ},
      {depth ? &depth->host : nullptr, plume::RenderTextureLayout::SHADER_READ},
      {&history_out, plume::RenderTextureLayout::COLOR_WRITE},
  };
  TransitionManyLocked(s, transitions, 4);

  c.indices[0] = BindTextureSRVLocked(s, scene->host);
  c.indices[1] = BindTextureSRVLocked(s, history_in);
  c.indices[2] = depth ? BindTextureSRVLocked(s, depth->host) : c.indices[0];
  c.indices[3] = depth ? 0u : 1u;

  UploadAlloc alloc;
  if (!UploadBytes(&c, sizeof(c), kConstantBufferAlignment, &alloc))
    return;

  HostTexture *colors[4] = {&history_out, nullptr, nullptr, nullptr};
  plume::RenderFramebuffer *fb = GetFramebuffer(s, colors, 1, nullptr);
  if (!fb)
    return;
  auto *cmd = s.command_list;
  {
    EOT_GPU_ZONE("taa resolve");
    GpuTimingMark(s, cmd, kGpuCatTaa);
    cmd->setFramebuffer(fb);
    s.bound_framebuffer = fb;
    s.bound_draw_targets_valid = false;
    cmd->setPipeline(st.pso.get());
    const plume::RenderViewport vp(0.0f, 0.0f, w, h, 0.0f, 1.0f);
    const plume::RenderRect sc(0, 0, static_cast<i32>(scene->host.width),
                               static_cast<i32>(scene->host.height));
    cmd->setViewports(&vp, 1);
    cmd->setScissors(&sc, 1);
    BindConstants(s, alloc);
    cmd->drawInstanced(3, 1, 0, 0);
    history_out.needsClear = false;

    const HostTextureTransition copy_in[] = {
        {&history_out, plume::RenderTextureLayout::COPY_SOURCE},
        {&scene->host, plume::RenderTextureLayout::COPY_DEST},
    };
    TransitionManyLocked(s, copy_in, 2);
    cmd->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(scene->host.texture.get(), 0, 0),
        plume::RenderTextureCopyLocation::Subresource(history_out.texture.get(), 0, 0));
    const HostTextureTransition copy_out[] = {
        {&scene->host, plume::RenderTextureLayout::SHADER_READ},
        {&history_out, plume::RenderTextureLayout::SHADER_READ},
    };
    TransitionManyLocked(s, copy_out, 2);
  }
  s.bound_pipeline = nullptr;
  s.bound_framebuffer = nullptr;
  s.bound_draw_targets_valid = false;

  st.write = read;
  st.historyValid = true;
  std::memcpy(st.prevViewProjection, camera_vp, sizeof(st.prevViewProjection));
  st.prevValid = true;
  st.lastFrame = s.guest_frames;
}

}
