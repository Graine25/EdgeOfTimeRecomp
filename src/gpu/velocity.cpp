#include "gpu/velocity.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>

#include <rex/memory/utils.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/gpu_profiling.h"
#include "gpu/gpu_timing.h"
#include "gpu/settings.h"
#include "gpu/surfaces.h"

#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#include "shaders/velocity_resolve_2x_ps.hlsl.dxil.h"
#include "shaders/velocity_resolve_4x_ps.hlsl.dxil.h"
#include "shaders/velocity_resolve_8x_ps.hlsl.dxil.h"
#else
#include "shaders/velocity_resolve_2x_ps.hlsl.spirv.h"
#include "shaders/velocity_resolve_4x_ps.hlsl.spirv.h"
#include "shaders/velocity_resolve_8x_ps.hlsl.spirv.h"
#endif

namespace eot::gpu::velocity {

namespace {

constexpr u32 kSlots = 4;
constexpr u32 kBlockBytes = (kSlots + 1) * kPrevFileOffset;
constexpr u64 kArenaChunkBytes = 16ull << 20;
constexpr u64 kBlockAlignment = 256;

struct ArenaChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *cpu = nullptr;
  u64 capacity = 0, used = 0, gpuVa = 0;
};

struct Block {
  plume::RenderBuffer *buffer = nullptr;
  u8 *cpu = nullptr;
  u64 offset = 0, gpuVa = 0;
  bool valid() const { return buffer != nullptr; }
};

struct Entry {
  u64 frame = ~0ull;
  u32 regs = 0;
  Block block;
};

struct CaptureState {
  u64 frame = 0;
  std::unordered_map<u64, Entry> store;
  std::unordered_map<u64, u32> ordinals;
  std::vector<ArenaChunk> chunks;
  std::vector<Block> freeBlocks;
  bool arenaFailed = false;
  u64 prepared = 0, paired = 0, firstSeen = 0, wrapCopies = 0;
};

CaptureState &capture() {
  static CaptureState c;
  return c;
}

constexpr u64 kSweepEvery = 300;
constexpr u64 kEntryTtl = 120;

u64 Mix(u64 a, u64 b) {
  a ^= b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2);
  return a;
}

bool MakeArenaChunk(VideoState &s, ArenaChunk &chunk) {
  plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(kArenaChunkBytes);
  desc.flags = plume::RenderBufferFlag::CONSTANT;
  chunk.buffer = CreateHostBuffer(s.device.get(), desc, "velocity-history");
  if (!chunk.buffer)
    return false;
  chunk.cpu = static_cast<u8 *>(chunk.buffer->map());
  if (!chunk.cpu) {
    chunk.buffer.reset();
    return false;
  }
  chunk.capacity = kArenaChunkBytes;
  chunk.used = 0;
#if defined(EOT_D3D12)
  chunk.gpuVa = static_cast<plume::D3D12Buffer *>(chunk.buffer.get())->d3d->GetGPUVirtualAddress();
#else
  chunk.gpuVa = chunk.buffer->getDeviceAddress();
  if (!chunk.gpuVa) {
    chunk.buffer->unmap();
    chunk.buffer.reset();
    return false;
  }
#endif
  return true;
}

bool AllocBlock(VideoState &s, CaptureState &c, Block *out) {
  if (!c.freeBlocks.empty()) {
    *out = c.freeBlocks.back();
    c.freeBlocks.pop_back();
    return true;
  }
  if (c.arenaFailed)
    return false;
  if (c.chunks.empty() || c.chunks.back().used + kBlockBytes + kBlockAlignment > c.chunks.back().capacity) {
    ArenaChunk chunk;
    if (!MakeArenaChunk(s, chunk)) {
      c.arenaFailed = true;
      EOT_ERROR("[velocity] history arena: a {} MB chunk failed; no more motion vectors this run",
                kArenaChunkBytes >> 20);
      return false;
    }
    c.chunks.push_back(std::move(chunk));
    EOT_INFO("[velocity] history arena: chunk {} ({} MB, {} blocks)", c.chunks.size(),
             kArenaChunkBytes >> 20, kArenaChunkBytes / kBlockBytes);
  }
  ArenaChunk &chunk = c.chunks.back();
  const u64 start = (chunk.used + kBlockAlignment - 1) / kBlockAlignment * kBlockAlignment;
  out->buffer = chunk.buffer.get();
  out->cpu = chunk.cpu + start;
  out->offset = start;
  out->gpuVa = chunk.gpuVa + start;
  chunk.used = start + kBlockBytes;
  return true;
}

struct Ring {
  u64 depthUid = 0;
  Target t[2];
  u32 current = 0;
  u64 lastUse = 0;
};

struct ReplayState {
  static constexpr u32 kRings = 4;
  Ring rings[kRings];
  std::unique_ptr<plume::RenderShader> resolvePs[3];
  std::unique_ptr<plume::RenderPipeline> resolvePso[3];
  bool shaderFailed = false;
  u64 passes = 0, clears = 0, draws = 0, resolves = 0, lastReport = 0;
};

ReplayState &replay() {
  static ReplayState r;
  return r;
}

Ring *FindRing(u64 uid) {
  for (Ring &r : replay().rings)
    if (r.depthUid == uid)
      return &r;
  return nullptr;
}

Ring &RingFor(VideoState &s, u64 uid) {
  if (Ring *r = FindRing(uid)) {
    r->lastUse = s.guest_frames;
    return *r;
  }
  Ring *pick = nullptr;
  for (Ring &r : replay().rings)
    if (!r.depthUid) {
      pick = &r;
      break;
    }
  if (!pick) {
    pick = &replay().rings[0];
    for (Ring &r : replay().rings)
      if (r.lastUse < pick->lastUse)
        pick = &r;
    for (Target &t : pick->t) {
      if (t.image.valid())
        ParkHostTexture(s, t.image);
      if (t.resolved.valid())
        ParkHostTexture(s, t.resolved);
    }
  }
  *pick = Ring{};
  pick->depthUid = uid;
  pick->lastUse = s.guest_frames;
  for (u32 i = 0; i < 2; ++i) {
    pick->t[i].depthUid = uid;
    pick->t[i].slot = i;
  }
  return *pick;
}

bool EnsureImage(VideoState &s, HostTexture &h, u32 width, u32 height, u32 samples, const char *tag) {
  if (h.valid() && h.width == width && h.height == height && h.sampleCount == samples)
    return true;
  if (h.valid())
    ParkHostTexture(s, h);
  plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(
      width, height, 1, kFormat, plume::RenderTextureFlag::RENDER_TARGET);
  desc.multisampling.sampleCount = static_cast<plume::RenderSampleCounts>(std::max(1u, samples));
  h.format = desc.format;
  h.width = width;
  h.height = height;
  h.depth = 1;
  h.mipLevels = 1;
  h.arraySize = 1;
  h.sampleCount = std::max(1u, samples);
  h.isDepth = false;
  h.renderable = true;
  if (!CreateOrRecycleHostTexture(s, h, desc, tag)) {
    EOT_ERROR("[velocity] {} image {}x{} x{} failed", tag, width, height, samples);
    return false;
  }
  return true;
}

bool EnsureResolvePipeline(VideoState &s, u32 samples) {
  ReplayState &r = replay();
  const int tier = samples == 2 ? 0 : samples == 4 ? 1 : samples == 8 ? 2 : -1;
  if (tier < 0 || r.shaderFailed)
    return false;
  if (r.resolvePso[tier])
    return true;
  if (!r.resolvePs[tier]) {
    switch (tier) {
    case 0:
      r.resolvePs[tier] = s.device->createShader(EOT_SHADER_BLOB(velocity_resolve_2x_ps), "main",
                                                 kHostShaderFormat);
      break;
    case 1:
      r.resolvePs[tier] = s.device->createShader(EOT_SHADER_BLOB(velocity_resolve_4x_ps), "main",
                                                 kHostShaderFormat);
      break;
    default:
      r.resolvePs[tier] = s.device->createShader(EOT_SHADER_BLOB(velocity_resolve_8x_ps), "main",
                                                 kHostShaderFormat);
      break;
    }
    if (!r.resolvePs[tier]) {
      r.shaderFailed = true;
      EOT_ERROR("[velocity] createShader (resolve {}x) failed; motion vectors off under MSAA", samples);
      return false;
    }
  }
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = s.pipeline_layout.get();
  desc.vertexShader = s.copy_vs.get();
  desc.pixelShader = r.resolvePs[tier].get();
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = false;
  desc.depthWriteEnabled = false;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = 1;
  desc.renderTargetFormat[0] = kFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  r.resolvePso[tier] = CreateHostGraphicsPipeline(s.device.get(), desc, "velocity-resolve");
  if (!r.resolvePso[tier]) {
    r.shaderFailed = true;
    EOT_ERROR("[velocity] resolve pipeline ({}x) failed; motion vectors off under MSAA", samples);
    return false;
  }
  return true;
}

bool ResolveLocked(VideoState &s, Target &t) {
  if (!s.command_list_open || !EnsureResolvePipeline(s, t.samples) ||
      !EnsureImage(s, t.resolved, t.width, t.height, 1, "velocity-resolved"))
    return false;
  const int tier = t.samples == 2 ? 0 : t.samples == 4 ? 1 : 2;
  FlushPendingTransitions(s);
  const HostTextureTransition transitions[] = {
      {&t.image, plume::RenderTextureLayout::SHADER_READ},
      {&t.resolved, plume::RenderTextureLayout::COLOR_WRITE},
  };
  TransitionManyLocked(s, transitions, 2);
  HostTexture *colors[4] = {&t.resolved, nullptr, nullptr, nullptr};
  plume::RenderFramebuffer *fb = GetFramebuffer(s, colors, 1, nullptr);
  if (!fb)
    return false;
  auto *cmd = s.command_list;
  {
    EOT_GPU_ZONE("velocity resolve");
    GpuTimingMark(s, cmd, kGpuCatTaa);
    cmd->setFramebuffer(fb);
    s.bound_framebuffer = fb;
    s.bound_draw_targets_valid = false;
    cmd->setPipeline(replay().resolvePso[tier].get());
    const plume::RenderViewport vp(0.0f, 0.0f, static_cast<float>(t.width),
                                   static_cast<float>(t.height), 0.0f, 1.0f);
    const plume::RenderRect sc(0, 0, static_cast<i32>(t.width), static_cast<i32>(t.height));
    cmd->setViewports(&vp, 1);
    cmd->setScissors(&sc, 1);
    CopyPushConstants pc;
    pc.resourceDescriptorIndex = BindTextureSRVLocked(s, t.image);
    pc.rect[0] = 0.0f;
    pc.rect[1] = 0.0f;
    pc.rect[2] = 1.0f;
    pc.rect[3] = 1.0f;
    cmd->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc, kCopyPushConstantByteOffset,
                                  sizeof(pc));
    cmd->drawInstanced(3, 1, 0, 0);
    t.resolved.needsClear = false;
  }
  const HostTextureTransition after[] = {
      {&t.resolved, plume::RenderTextureLayout::SHADER_READ},
  };
  TransitionManyLocked(s, after, 1);
  s.bound_pipeline = nullptr;
  s.bound_framebuffer = nullptr;
  s.bound_draw_targets_valid = false;
  replay().resolves++;
  return true;
}

}

void BeginCaptureFrame(VideoState &) {
  CaptureState &c = capture();
  ++c.frame;
  c.ordinals.clear();
  if (c.frame % kSweepEvery == 0) {
    for (auto it = c.store.begin(); it != c.store.end();) {
      if (c.frame - it->second.frame > kEntryTtl) {
        if (it->second.block.valid())
          c.freeBlocks.push_back(it->second.block);
        it = c.store.erase(it);
      } else {
        ++it;
      }
    }
  }
}

bool PrepareDraw(VideoState &s, u64 key, const u8 *guest_file, u32 regs, UploadAlloc *out) {
  CaptureState &c = capture();
  regs = std::clamp(regs, 16u, 256u);
  const u32 bytes = regs * 16;
  const u32 ordinal = c.ordinals[key]++;
  const u64 id = Mix(key, ordinal);
  Entry &e = c.store[id];
  c.prepared++;
  if (!e.block.valid()) {
    c.firstSeen++;
    if (!AllocBlock(s, c, &e.block))
      return false;
  }
  const u32 slot = (kSlots - 1) - static_cast<u32>(c.frame % kSlots);
  const bool paired = e.frame + 1 == c.frame && e.regs >= regs;
  if (paired && slot == kSlots - 1) {
    std::memcpy(e.block.cpu + kSlots * kPrevFileOffset, e.block.cpu, e.regs * 16);
    c.wrapCopies++;
  }
  u8 *cur = e.block.cpu + slot * kPrevFileOffset;
  rex::memory::copy_and_swap_32_unaligned(cur, reinterpret_cast<const u32 *>(guest_file), regs * 4);
  e.regs = regs;
  e.frame = c.frame;
  if (!paired)
    return false;
  c.paired++;
  *out = UploadAlloc{};
  out->buffer = e.block.buffer;
  out->offset = e.block.offset + slot * kPrevFileOffset;
  out->cpu = cur;
  out->size = 2 * kPrevFileOffset;
  out->gpuVa = e.block.gpuVa + slot * kPrevFileOffset;
  static u64 logged = ~0ull;
  if (Settings::DiagVerbosity() >= 2 && logged != c.frame / 600) {
    logged = c.frame / 600;
    const float *cf = reinterpret_cast<const float *>(cur);
    const float *pf = reinterpret_cast<const float *>(cur + kPrevFileOffset);
    EOT_DEBUG("[velocity] pair frame {} key {:016x} ord {} regs {} slot {} cur c0 {:.5g} {:.5g} {:.5g} "
              "{:.5g} | prev c0 {:.5g} {:.5g} {:.5g} {:.5g} | gpu {:#x}",
              c.frame, key, ordinal, regs, slot, cf[0], cf[1], cf[2], cf[3], pf[0], pf[1], pf[2], pf[3],
              out->gpuVa);
  }
  return true;
}

Target *CurrentFor(VideoState &s, const GuestSurface &depth, u32 width, u32 height, u32 samples) {
  if (!width || !height)
    return nullptr;
  Ring &r = RingFor(s, depth.uid);
  Target &t = r.t[r.current];
  const bool recreate = !t.image.valid() || t.width != width || t.height != height ||
                        t.samples != std::max(1u, samples);
  if (recreate) {
    if (!EnsureImage(s, t.image, width, height, samples, "velocity"))
      return nullptr;
    t.width = width;
    t.height = height;
    t.samples = std::max(1u, samples);
    t.generation++;
    t.needsClear = true;
    t.frameWritten = ~0ull;
    t.frameResolved = ~0ull;
    EOT_INFO("[velocity] target {}x{} x{} for depth surface {:#x} slot {}", width, height,
             t.samples, depth.va, t.slot);
  }
  return &t;
}

void BeforeDraw(VideoState &s, Target &t, u32 attachment) {
  ReplayState &r = replay();
  if (t.needsClear) {
    s.command_list->clearColor(attachment, plume::RenderColor(kSentinel, 0.0f, 0.0f, 0.0f),
                               nullptr, 0);
    t.image.needsClear = false;
    t.needsClear = false;
    r.passes++;
  }
  t.frameWritten = s.guest_frames;
  r.draws++;
}

void OnDepthCleared(VideoState &, const GuestSurface &depth) {
  Ring *r = FindRing(depth.uid);
  if (!r)
    return;
  r->current ^= 1u;
  r->t[r->current].needsClear = true;
  replay().clears++;
}

VelocityHandle HandleFor(const GuestSurface &depth, u64 frame) {
  VelocityHandle h;
  const Ring *r = FindRing(depth.uid);
  if (!r)
    return h;
  const Target &t = r->t[r->current];
  if (t.frameWritten != frame || !t.image.valid())
    return h;
  h.depthUid = t.depthUid;
  h.slot = t.slot;
  h.generation = t.generation;
  h.frame = frame;
  return h;
}

HostTexture *ResolvedImage(VideoState &s, const VelocityHandle &h, u64 frame) {
  if (!h.depthUid || h.frame != frame)
    return nullptr;
  Ring *r = FindRing(h.depthUid);
  if (!r)
    return nullptr;
  Target &t = r->t[h.slot & 1u];
  if (t.generation != h.generation || t.frameWritten != frame || !t.image.valid())
    return nullptr;
  HostTexture *image = &t.image;
  if (t.samples > 1) {
    if (t.frameResolved != frame) {
      if (!ResolveLocked(s, t))
        return nullptr;
      t.frameResolved = frame;
    }
    image = &t.resolved;
  }
  const i32 every = Settings::DumpEvery();
  if (every > 0 && frame % static_cast<u64>(every) == 0) {
    const std::string path_s = std::format("logs/velocity_{}_s.ppm", frame);
    const std::string path_l = std::format("logs/velocity_{}_l.ppm", frame);
    DumpHostTextureLocked(s, *image, path_s.c_str(), 0.1f);
    DumpHostTextureLocked(s, *image, path_l.c_str(), 64.0f);
  }
  return image;
}

void EndFrame(VideoState &s) {
  ReplayState &r = replay();
  if (s.guest_frames - r.lastReport < 600)
    return;
  r.lastReport = s.guest_frames;
  CaptureState &c = capture();
  if (!c.prepared && !r.draws)
    return;
  EOT_DEBUG("[velocity] 600 frames: {} draws prepared, {} paired with last frame, {} first seen, "
            "{} wrap copies, store {} entries in {} arena chunk(s) ({} free blocks); {} passes, {} "
            "depth clears, {} velocity draws, {} resolves",
            c.prepared, c.paired, c.firstSeen, c.wrapCopies, c.store.size(), c.chunks.size(),
            c.freeBlocks.size(), r.passes, r.clears, r.draws, r.resolves);
  c.prepared = c.paired = c.firstSeen = c.wrapCopies = 0;
  r.passes = r.clears = r.draws = r.resolves = 0;
}

void Shutdown(VideoState &s) {
  ReplayState &r = replay();
  for (Ring &ring : r.rings) {
    for (Target &t : ring.t) {
      if (t.image.valid())
        ParkHostTexture(s, t.image);
      if (t.resolved.valid())
        ParkHostTexture(s, t.resolved);
    }
    ring = Ring{};
  }
  for (auto &p : r.resolvePso)
    p.reset();
  for (auto &p : r.resolvePs)
    p.reset();
  CaptureState &c = capture();
  c.store.clear();
  c.freeBlocks.clear();
  for (ArenaChunk &chunk : c.chunks)
    if (chunk.buffer)
      chunk.buffer->unmap();
  c.chunks.clear();
}

}
