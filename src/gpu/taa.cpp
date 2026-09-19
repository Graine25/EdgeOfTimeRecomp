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
#include "gpu/velocity.h"

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
  float motion[4];
  u32 indices2[4];
};
static_assert(sizeof(Constants) == 144);

constexpr float kFeedbackZero = 0.012f;
constexpr float kSkipAbove = 0.02f;
constexpr float kResumeBelow = 0.01f;
constexpr u32 kResumeFrames = 4;

struct View {
  u32 mirrorVa = 0;
  HostTexture history[2];
  u32 write = 0;
  bool historyValid = false;
  float prevViewProjection[16] = {};
  bool prevValid = false;
  u64 lastFrame = ~0ull;
  u64 lastUseFrame = 0;
  u32 width = 0, height = 0;
};

struct State {
  std::unique_ptr<plume::RenderShader> ps;
  std::unique_ptr<plume::RenderPipeline> pso;
  plume::RenderFormat psoFormat = plume::RenderFormat::UNKNOWN;
  static constexpr u32 kMaxViews = 4;
  View views[kMaxViews];
  u64 lastFrame = ~0ull;
  bool shaderFailed = false;
  u64 jitteredFrame = ~0ull;
  u64 framesResolved = 0, framesJitteredUnresolved = 0, framesPassThrough = 0, framesSkipped = 0;
  u64 framesWithVectors = 0;
  u64 lastReport = 0;
  u64 consumerHash = 0;
  bool lastHadDepth = false;
  bool noConsumer = false;
  u64 latchFrame = ~0ull;
  bool latchSkip = false;
  bool latchOn = false;
  u32 latchIndex = 0;
  float latchX = 0.0f, latchY = 0.0f;
  u64 skipMismatches = 0;
  u64 drawsAfterResolve = 0;
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

}

float CameraMotionPixels(const float prev_vp[16], const float cur_vp[16], float width, float height) {
  float inv[16], r[16];
  if (!Invert(cur_vp, inv))
    return 0.0f;
  Multiply(inv, prev_vp, r);
  const float clip[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float p[4];
  for (int c = 0; c < 4; ++c)
    p[c] = clip[0] * r[0 * 4 + c] + clip[1] * r[1 * 4 + c] + clip[2] * r[2 * 4 + c] +
           clip[3] * r[3 * 4 + c];
  if (!(p[3] > 1e-6f))
    return 1e9f;
  const float dx = p[0] / p[3] * 0.5f * width, dy = p[1] / p[3] * 0.5f * height;
  return std::sqrt(dx * dx + dy * dy);
}

bool FastCameraGate(u32 slot, float motion_px, float width) {
  static struct {
    bool skipping = false;
    u32 calm = 0;
  } gates[4];
  auto &g = gates[slot & 3];
  if (g.skipping) {
    if (motion_px < kResumeBelow * width) {
      if (++g.calm >= kResumeFrames)
        g.skipping = false;
    } else {
      g.calm = 0;
    }
  } else if (motion_px > kSkipAbove * width) {
    g.skipping = true;
    g.calm = 0;
  }
  return g.skipping;
}

namespace {

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

bool EnsureHistory(VideoState &s, View &st, const HostTexture &scene) {
  if (st.width == scene.width && st.height == scene.height && st.history[0].valid() &&
      st.history[1].valid())
    return true;
  for (HostTexture &h : st.history)
    if (h.valid())
      ParkHostTexture(s, h);
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      scene.width, scene.height, 1, scene.format, plume::RenderTextureFlag::RENDER_TARGET);
  for (HostTexture &h : st.history) {
    h.format = desc.format;
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
  st.width = scene.width;
  st.height = scene.height;
  st.historyValid = false;
  st.prevValid = false;
  EOT_INFO("[taa] history {}x{} for scene {:#x} ({} phases, feedback {:.2f})", desc.width,
           desc.height, st.mirrorVa, kJitterPhases, Settings::TaaFeedback());
  return true;
}

View &ViewFor(VideoState &s, State &st, const GuestTexture &scene) {
  for (View &v : st.views)
    if (v.mirrorVa == scene.va)
      return v;
  View *pick = nullptr;
  for (View &v : st.views)
    if (!v.mirrorVa) {
      pick = &v;
      break;
    }
  if (!pick) {
    pick = &st.views[0];
    for (View &v : st.views)
      if (v.lastUseFrame < pick->lastUseFrame)
        pick = &v;
    for (HostTexture &h : pick->history)
      if (h.valid())
        ParkHostTexture(s, h);
  }
  *pick = View{};
  pick->mirrorVa = scene.va;
  return *pick;
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

namespace {

void Latch(const VideoState &s, bool packet_skip, bool rect_list) {
  State &st = state();
  if (st.latchFrame == s.guest_frames) {
    if (!rect_list && packet_skip != st.latchSkip)
      st.skipMismatches++;
    return;
  }
  if (rect_list)
    return;
  st.latchFrame = s.guest_frames;
  st.latchSkip = packet_skip;
  st.latchOn = Settings::Taa() && !st.noConsumer && !packet_skip;
  const u32 phase = static_cast<u32>(s.guest_frames % kJitterPhases);
  st.latchIndex = st.latchOn ? 1u + phase : 0u;
  st.latchX = st.latchOn ? Halton(phase, 2) - 0.5f : 0.0f;
  st.latchY = st.latchOn ? Halton(phase, 3) - 0.5f : 0.0f;
}

}

bool ResolvedThisFrame(const VideoState &s) { return state().lastFrame == s.guest_frames; }

u32 JitterIndex(const VideoState &s, bool packet_skip, bool rect_list) {
  Latch(s, packet_skip, rect_list);
  const State &st = state();
  if (st.latchFrame != s.guest_frames || ResolvedThisFrame(s))
    return 0u;
  return st.latchIndex;
}

void FrameJitter(const VideoState &s, bool packet_skip, bool rect_list, float *jx, float *jy) {
  Latch(s, packet_skip, rect_list);
  State &st = state();
  *jx = 0.0f;
  *jy = 0.0f;
  if (st.latchFrame != s.guest_frames || !st.latchOn || rect_list)
    return;
  if (ResolvedThisFrame(s)) {
    st.drawsAfterResolve++;
    return;
  }
  *jx = st.latchX;
  *jy = st.latchY;
  st.jitteredFrame = s.guest_frames;
}

bool FrameSkip(const VideoState &s, bool packet_skip) {
  const State &st = state();
  return st.latchFrame == s.guest_frames ? st.latchSkip : packet_skip;
}

void EndFrame(VideoState &s) {
  State &st = state();
  if (!Settings::Taa())
    return;
  const u64 frame = s.guest_frames;
  if (st.jitteredFrame == frame) {
    if (st.lastFrame == frame) {
      st.framesResolved++;
    } else {
      st.framesJitteredUnresolved++;
      st.noConsumer = true;
    }
  }
  if (frame - st.lastReport >= 600) {
    st.lastReport = frame;
    EOT_DEBUG("[taa] {} frames: {} resolved ({} with motion vectors), {} jittered without a "
              "resolve, {} resolved without history, {} skipped for a fast camera, {} draws whose "
              "skip disagreed with the frame's latch, {} draws after the resolve left unjittered; "
              "last consumer {:#x}{}",
              600, st.framesResolved, st.framesWithVectors, st.framesJitteredUnresolved,
              st.framesPassThrough, st.framesSkipped, st.skipMismatches, st.drawsAfterResolve,
              st.consumerHash, st.lastHadDepth ? "" : " (no depth mirror bound)");
    st.framesResolved = st.framesJitteredUnresolved = st.framesPassThrough = st.framesSkipped = 0;
    st.framesWithVectors = 0;
    st.skipMismatches = 0;
    st.drawsAfterResolve = 0;
  }
}

void Reset(VideoState &) {
  for (View &v : state().views) {
    v.historyValid = false;
    v.prevValid = false;
  }
}

void Shutdown(VideoState &s) {
  State &st = state();
  for (View &v : st.views) {
    for (HostTexture &h : v.history)
      if (h.valid())
        ParkHostTexture(s, h);
    v = View{};
  }
  st.pso.reset();
  st.ps.reset();
  st.psoFormat = plume::RenderFormat::UNKNOWN;
}

void BeforeSceneConsumerDraw(VideoState &s, GuestTexture *const bound[16], const float *camera_vp,
                             u64 consumer_hash, bool skip) {
  State &state_ = state();
  state_.consumerHash = consumer_hash;
  GuestTexture *scene = bound[0];
  if (camera_vp && scene && Settings::DiagVerbosity() >= 2 && state_.lastFrame != s.guest_frames) {
    EOT_DEBUG("[taa] frame {} scene {:#x} vp {:.6g} {:.6g} {:.6g} {:.6g} | {:.6g} {:.6g} {:.6g} {:.6g} | "
              "{:.6g} {:.6g} {:.6g} {:.6g} | {:.6g} {:.6g} {:.6g} {:.6g}",
              s.guest_frames, scene->va, camera_vp[0], camera_vp[1], camera_vp[2], camera_vp[3],
              camera_vp[4], camera_vp[5], camera_vp[6], camera_vp[7], camera_vp[8], camera_vp[9],
              camera_vp[10], camera_vp[11], camera_vp[12], camera_vp[13], camera_vp[14], camera_vp[15]);
    if (!Settings::Taa())
      state_.lastFrame = s.guest_frames;
  }
  if (!Settings::Taa() || !camera_vp) {
    Reset(s);
    return;
  }
  if (!IsFullFrameSceneMirror(scene))
    return;
  state_.noConsumer = false;
  View &st = ViewFor(s, state_, *scene);
  st.lastUseFrame = s.guest_frames;
  if (st.lastFrame == s.guest_frames)
    return;
  skip = FrameSkip(s, skip);
  if (skip) {
    st.historyValid = false;
    st.prevValid = false;
    st.lastFrame = s.guest_frames;
    state_.lastFrame = s.guest_frames;
    state_.framesSkipped++;
    return;
  }
  GuestTexture *depth = nullptr;
  for (u32 i = 1; i < 16 && !depth; ++i)
    if (IsFullFrameDepthMirror(bound[i]))
      depth = bound[i];
  if (!s.command_list_open || !EnsureShader(s, state_, scene->host.format) ||
      !EnsureHistory(s, st, scene->host))
    return;
  HostTexture *vectors = depth && Settings::MotionVectors()
                             ? velocity::ResolvedImage(s, depth->velocity, s.guest_frames)
                             : nullptr;
  if (vectors)
    state_.framesWithVectors++;

  Constants c{};
  float inv[16];
  const bool reproject_ok = st.prevValid && Invert(camera_vp, inv);
  if (reproject_ok)
    Multiply(inv, st.prevViewProjection, c.reproject);
  float jx, jy;
  FrameJitter(s, skip, false, &jx, &jy);
  const float w = static_cast<float>(scene->host.width), h = static_cast<float>(scene->host.height);
  c.jitter[0] = jx / w;
  c.jitter[1] = jy / h;
  c.jitter[2] = 1.0f / w;
  c.jitter[3] = 1.0f / h;
  c.params[0] = static_cast<float>(Settings::TaaFeedback());
  c.params[1] = st.historyValid && reproject_ok && depth ? 1.0f : 0.0f;
  state_.lastHadDepth = depth != nullptr;
  if (c.params[1] == 0.0f)
    state_.framesPassThrough++;
  c.params[2] = w;
  c.params[3] = h;
  c.motion[0] = kFeedbackZero * w;

  const u32 read = st.write ^ 1u;
  HostTexture &history_in = st.history[read];
  HostTexture &history_out = st.history[st.write];

  FlushPendingTransitions(s);
  const HostTextureTransition transitions[] = {
      {&scene->host, plume::RenderTextureLayout::SHADER_READ},
      {&history_in, plume::RenderTextureLayout::SHADER_READ},
      {depth ? &depth->host : nullptr, plume::RenderTextureLayout::SHADER_READ},
      {&history_out, plume::RenderTextureLayout::COLOR_WRITE},
      {vectors, plume::RenderTextureLayout::SHADER_READ},
  };
  TransitionManyLocked(s, transitions, 5);

  c.indices[0] = BindTextureSRVLocked(s, scene->host);
  c.indices[1] = BindTextureSRVLocked(s, history_in);
  c.indices[2] = depth ? BindTextureSRVLocked(s, depth->host) : c.indices[0];
  c.indices[3] = depth ? 0u : 1u;
  c.indices2[0] = vectors ? BindTextureSRVLocked(s, *vectors) : 0u;
  c.indices2[1] = vectors ? 1u : 0u;

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
    cmd->setPipeline(state_.pso.get());
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
  state_.lastFrame = s.guest_frames;
}

}
