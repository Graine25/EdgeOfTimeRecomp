#include "gpu/pipeline/pipeline_cache.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/hash.h>

#include <rex/cvar.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"
#include "gpu/device/device.h"
#include "gpu/guest/resources.h"
#include "gpu/pipeline/vertex_layout.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/shaders/shader_cache.h"

namespace eot::gpu {

constexpr u32 kSpecConstantAlphaTest = 1u << 1;
constexpr u32 kAlphaTestEnableBit = 1u << 3;

namespace {

std::mutex g_pipeline_mutex;

struct Entry {
  std::unique_ptr<plume::RenderPipeline> pipeline;
  u32 draws = 0;
};
std::unordered_map<PipelineKey, Entry, PipelineKeyHash> g_pipelines;

std::atomic<u32> g_lookups{0};
std::atomic<u32> g_undescribable{0};
std::atomic<u32> g_layout_ok{0};
std::atomic<u32> g_layout_failed{0};
std::atomic<u32> g_layout_reported{0};
std::atomic<u32> g_layout_no_fetches{0};
std::atomic<u32> g_layout_no_decl{0};
std::atomic<u32> g_layout_join_failed{0};
std::atomic<u32> g_built{0};
std::atomic<u32> g_build_failed{0};

void LogPipelineStatsLocked();

void Mix(u64 &h, u64 value) {
  h ^= value + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
}

}

void NoteInputLayout(const InputLayout &layout, bool ok) {
  if (ok) {
    if (g_layout_ok.fetch_add(1, std::memory_order_relaxed) == 0) {
      char buf[320];
      int len = 0;
      for (u32 i = 0; i < layout.count && len < int(sizeof(buf)) - 40; ++i) {
        const auto &e = layout.elements[i];
        len += snprintf(buf + len, sizeof(buf) - len, "%s%s%u@s%u+%u:f%u",
                        i ? " " : "", VertexUsageName(e.usage), e.usageIndex,
                        e.stream, e.offset, static_cast<u32>(e.format));
      }
      EOT_INFO("[pso] first input layout: {} elements - {}", layout.count, buf);
    }
  } else {
    g_layout_failed.fetch_add(1, std::memory_order_relaxed);
  }
}

size_t PipelineKeyHash::operator()(const PipelineKey &k) const {
  u64 h = 0;
  Mix(h, k.vertexShaderHash);
  Mix(h, k.pixelShaderHash);
  Mix(h, (u64(k.vertexSpecConstants) << 32) | k.pixelSpecConstants);
  Mix(h, (u64(static_cast<u32>(k.renderTargetFormat)) << 32) |
             static_cast<u32>(k.depthFormat));
  Mix(h, (u64(static_cast<u32>(k.topology)) << 32) | k.sampleCount);
  Mix(h, k.inputLayoutHash);
  Mix(h, k.stateHash);
  return static_cast<size_t>(h);
}

u64 HashInputLayout(const InputLayout &layout) {
  u64 h = 0;
  for (u32 i = 0; i < layout.count; ++i) {
    const auto &e = layout.elements[i];
    Mix(h, (u64(static_cast<u32>(e.usage)) << 40) | (u64(e.usageIndex) << 32) |
               (u64(e.stream) << 24) | e.offset);
    Mix(h, static_cast<u32>(e.format));
  }
  return h;
}

bool BuildPipelineKeyForCurrentState(u32 device_va, PipelineKey &out) {
  GuestShader *vs = Video::BoundVertexShader();
  if (!vs || !vs->shaderCacheEntry)
    return false;

  out = PipelineKey{};
  out.vertexShaderHash = vs->hash;

  VertexLayout fetches;
  VertexDeclaration decl;
  InputLayout input;
  if (!DecodeVertexLayout(vs, fetches)) {
    g_layout_no_fetches.fetch_add(1, std::memory_order_relaxed);
    NoteInputLayout(input, false);
  } else if (!CurrentVertexDeclaration(device_va, decl)) {
    g_layout_no_decl.fetch_add(1, std::memory_order_relaxed);
    NoteInputLayout(input, false);
  } else if (!BuildInputLayout(fetches, decl, input)) {
    g_layout_join_failed.fetch_add(1, std::memory_order_relaxed);
    NoteInputLayout(input, false);
  } else {
    NoteInputLayout(input, true);
    out.inputLayoutHash = HashInputLayout(input);
    out.layout = input;
  }
  const u32 color_control = mem::try_load<u32>(device_va + kColorControlOffset);
  const u32 depth_control = mem::try_load<u32>(device_va + kDepthControlOffset);
  const u32 blend_control = mem::try_load<u32>(device_va + kBlendControl0Offset);
  const bool alpha_test = (color_control & kAlphaTestEnableBit) != 0;

  u32 spec = 0;
  if (alpha_test)
    spec |= kSpecConstantAlphaTest;

  out.stateHash = (u64(depth_control) << 32) ^ blend_control ^
                  (u64(color_control) << 16) ^
                  (u64(out.modeControl) << 8) ^ (u64(out.colorMask) << 48) ^
                  (u64(out.stencilRefMask) << 24) ^
                  (u64(out.polyOffsetScale) << 4) ^ u64(out.polyOffsetBias);
  out.blendControl = blend_control;
  out.depthControl = depth_control;
  out.modeControl = mem::try_load<u32>(device_va + kModeControlOffset);
  out.colorMask = mem::try_load<u32>(device_va + kColorMaskOffset);
  out.stencilRefMask = mem::try_load<u32>(device_va + kStencilRefMaskOffset);
  {
    const rex::graphics::reg::PA_SU_SC_MODE_CNTL mode{out.modeControl};
    const bool use_back = mode.poly_offset_back_enable && !mode.poly_offset_front_enable;
    const u32 scale_at = use_back ? kPolyOffsetBackScaleOffset
                                  : kPolyOffsetFrontScaleOffset;
    const u32 bias_at = use_back ? kPolyOffsetBackOffset : kPolyOffsetFrontOffset;
    if (mode.poly_offset_front_enable || mode.poly_offset_back_enable) {
      out.polyOffsetScale = mem::try_load<u32>(device_va + scale_at);
      out.polyOffsetBias = mem::try_load<u32>(device_va + bias_at);
    }
  }
  const Video::AttachmentFormats fmts = Video::BoundAttachmentFormats();
  if (fmts.color == plume::RenderFormat::UNKNOWN &&
      fmts.depth == plume::RenderFormat::UNKNOWN)
    return false;
  out.renderTargetFormat = fmts.color;
  out.depthFormat = fmts.depth;
  if (const GuestTexture *ds = Video::BoundDepthTexture()) {
    constexpr u32 kD3DFMT_D24FS8 = 0x1A220197;
    out.reverseZ = false;
    (void)kD3DFMT_D24FS8;
    (void)ds;
  }
  out.sampleCount = fmts.sampleCount;
  return true;
}

namespace {

constexpr bool kEnableDepthTest = true;

plume::RenderStencilOp ConvertStencilOp(rex::graphics::xenos::StencilOp op) {
  using SO = rex::graphics::xenos::StencilOp;
  using RS = plume::RenderStencilOp;
  switch (op) {
  case SO::kZero:
    return RS::ZERO;
  case SO::kReplace:
    return RS::REPLACE;
  case SO::kIncrementClamp:
    return RS::INCREMENT_AND_CLAMP;
  case SO::kDecrementClamp:
    return RS::DECREMENT_AND_CLAMP;
  case SO::kInvert:
    return RS::INVERT;
  case SO::kIncrementWrap:
    return RS::INCREMENT_AND_WRAP;
  case SO::kDecrementWrap:
    return RS::DECREMENT_AND_WRAP;
  case SO::kKeep:
  default:
    return RS::KEEP;
  }
}

plume::RenderComparisonFunction
ConvertCompareFunction(rex::graphics::xenos::CompareFunction f, bool reverse_z) {
  using CF = rex::graphics::xenos::CompareFunction;
  using RC = plume::RenderComparisonFunction;
  switch (f) {
  case CF::kNever:
    return RC::NEVER;
  case CF::kLess:
    return reverse_z ? RC::GREATER : RC::LESS;
  case CF::kEqual:
    return RC::EQUAL;
  case CF::kLessEqual:
    return reverse_z ? RC::GREATER_EQUAL : RC::LESS_EQUAL;
  case CF::kGreater:
    return reverse_z ? RC::LESS : RC::GREATER;
  case CF::kNotEqual:
    return RC::NOT_EQUAL;
  case CF::kGreaterEqual:
    return reverse_z ? RC::LESS_EQUAL : RC::GREATER_EQUAL;
  case CF::kAlways:
  default:
    return RC::ALWAYS;
  }
}

constexpr u32 kBlendColorSrcShift = 0;
constexpr u32 kBlendColorCombShift = 5;
constexpr u32 kBlendColorDstShift = 8;
constexpr u32 kBlendAlphaSrcShift = 16;
constexpr u32 kBlendAlphaCombShift = 21;
constexpr u32 kBlendAlphaDstShift = 24;

plume::RenderBlend ConvertBlendFactor(u32 factor) {
  switch (static_cast<rex::graphics::xenos::BlendFactor>(factor)) {
  case rex::graphics::xenos::BlendFactor::kZero:
    return plume::RenderBlend::ZERO;
  case rex::graphics::xenos::BlendFactor::kOne:
    return plume::RenderBlend::ONE;
  case rex::graphics::xenos::BlendFactor::kSrcColor:
    return plume::RenderBlend::SRC_COLOR;
  case rex::graphics::xenos::BlendFactor::kOneMinusSrcColor:
    return plume::RenderBlend::INV_SRC_COLOR;
  case rex::graphics::xenos::BlendFactor::kSrcAlpha:
    return plume::RenderBlend::SRC_ALPHA;
  case rex::graphics::xenos::BlendFactor::kOneMinusSrcAlpha:
    return plume::RenderBlend::INV_SRC_ALPHA;
  case rex::graphics::xenos::BlendFactor::kDstColor:
    return plume::RenderBlend::DEST_COLOR;
  case rex::graphics::xenos::BlendFactor::kOneMinusDstColor:
    return plume::RenderBlend::INV_DEST_COLOR;
  case rex::graphics::xenos::BlendFactor::kDstAlpha:
    return plume::RenderBlend::DEST_ALPHA;
  case rex::graphics::xenos::BlendFactor::kOneMinusDstAlpha:
    return plume::RenderBlend::INV_DEST_ALPHA;
  case rex::graphics::xenos::BlendFactor::kSrcAlphaSaturate:
    return plume::RenderBlend::SRC_ALPHA_SAT;
  default:
    return plume::RenderBlend::ONE;
  }
}

plume::RenderBlendOperation ConvertBlendOp(u32 op) {
  switch (op) {
  case 1:
    return plume::RenderBlendOperation::SUBTRACT;
  case 2:
    return plume::RenderBlendOperation::MIN;
  case 3:
    return plume::RenderBlendOperation::MAX;
  case 4:
    return plume::RenderBlendOperation::REV_SUBTRACT;
  default:
    return plume::RenderBlendOperation::ADD;
  }
}

plume::RenderBlendDesc ConvertBlend(u32 blend_control) {
  const u32 color_src = (blend_control >> kBlendColorSrcShift) & 0x1F;
  const u32 color_dst = (blend_control >> kBlendColorDstShift) & 0x1F;
  const u32 color_comb = (blend_control >> kBlendColorCombShift) & 0x7;
  const u32 alpha_src = (blend_control >> kBlendAlphaSrcShift) & 0x1F;
  const u32 alpha_dst = (blend_control >> kBlendAlphaDstShift) & 0x1F;
  const u32 alpha_comb = (blend_control >> kBlendAlphaCombShift) & 0x7;

  plume::RenderBlendDesc desc = plume::RenderBlendDesc::Copy();
  const bool opaque =
      color_src == u32(rex::graphics::xenos::BlendFactor::kOne) &&
      color_dst == u32(rex::graphics::xenos::BlendFactor::kZero) &&
      color_comb == 0 &&
      alpha_src == u32(rex::graphics::xenos::BlendFactor::kOne) &&
      alpha_dst == u32(rex::graphics::xenos::BlendFactor::kZero) &&
      alpha_comb == 0;
  if (opaque)
    return desc;

  desc.blendEnabled = true;
  desc.srcBlend = ConvertBlendFactor(color_src);
  desc.dstBlend = ConvertBlendFactor(color_dst);
  desc.blendOp = ConvertBlendOp(color_comb);
  desc.srcBlendAlpha = ConvertBlendFactor(alpha_src);
  desc.dstBlendAlpha = ConvertBlendFactor(alpha_dst);
  desc.blendOpAlpha = ConvertBlendOp(alpha_comb);
  return desc;
}

std::unique_ptr<plume::RenderPipeline>
BuildPipeline(const PipelineKey &key, const InputLayout &layout) {
  auto *device = Video::HostDevice();
  GuestShader *vs = Video::BoundVertexShader();
  GuestShader *ps = Video::BoundPixelShader();
  if (!device || !vs)
    return nullptr;

  plume::RenderShader *host_vs = GetOrLinkShader(vs, key.vertexSpecConstants);
  if (!host_vs)
    return nullptr;
  plume::RenderShader *host_ps =
      ps ? GetOrLinkShader(ps, key.pixelSpecConstants) : nullptr;

  plume::RenderInputElement elements[kMaxVertexFetches]{};
  for (u32 i = 0; i < layout.count; ++i) {
    const auto &e = layout.elements[i];
    elements[i] = plume::RenderInputElement(VertexUsageSemantic(e.usage),
                                            e.usageIndex, i,
                                            e.format, e.stream, e.offset);
  }

  plume::RenderInputSlot slots[kMaxStreamSources]{};
  u32 slot_count = 0;
  for (u32 i = 0; i < layout.count; ++i) {
    const u32 stream = layout.elements[i].stream;
    bool seen = false;
    for (u32 j = 0; j < slot_count; ++j)
      seen = seen || slots[j].index == stream;
    if (seen)
      continue;
    const u32 stride = Video::BoundStreamStride(stream);
    if (stride == 0)
      return nullptr;
    slots[slot_count++] = plume::RenderInputSlot(
        stream, stride, plume::RenderInputSlotClassification::PER_VERTEX_DATA);
  }

  plume::RenderPipelineLayout *layout_obj = Video::GuestPipelineLayout();
  if (!layout_obj)
    return nullptr;

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_obj;
  desc.vertexShader = host_vs;
  desc.pixelShader = host_ps;
  desc.inputElements = elements;
  desc.inputElementsCount = layout.count;
  desc.inputSlots = slots;
  desc.inputSlotsCount = slot_count;
  desc.primitiveTopology = key.topology;
  desc.multisampling.sampleCount = key.sampleCount;
  desc.depthTargetFormat = key.depthFormat;

  if (key.depthFormat != plume::RenderFormat::UNKNOWN && kEnableDepthTest) {
    const rex::graphics::reg::RB_DEPTHCONTROL dc{key.depthControl};
    desc.depthEnabled = dc.z_enable != 0;
    desc.depthWriteEnabled = dc.z_write_enable != 0;
    desc.depthFunction = ConvertCompareFunction(dc.zfunc, key.reverseZ);
  }
  if (key.renderTargetFormat != plume::RenderFormat::UNKNOWN) {
    desc.renderTargetFormat[0] = key.renderTargetFormat;
    desc.renderTargetBlend[0] = ConvertBlend(key.blendControl);
    desc.renderTargetBlend[0].renderTargetWriteMask =
        static_cast<u8>(key.colorMask & 0xFu);
    desc.renderTargetCount = 1;
  }
  if (key.depthFormat != plume::RenderFormat::UNKNOWN) {
    const rex::graphics::reg::RB_DEPTHCONTROL dc{key.depthControl};
    desc.stencilEnabled = dc.stencil_enable != 0;
    if (desc.stencilEnabled) {
      desc.stencilReference = key.stencilRefMask & 0xFFu;
      desc.stencilReadMask = (key.stencilRefMask >> 8) & 0xFFu;
      desc.stencilWriteMask = (key.stencilRefMask >> 16) & 0xFFu;

      desc.stencilFrontFace.compareFunction =
          ConvertCompareFunction(dc.stencilfunc, key.reverseZ);
      desc.stencilFrontFace.failOp = ConvertStencilOp(dc.stencilfail);
      desc.stencilFrontFace.passOp = ConvertStencilOp(dc.stencilzpass);
      desc.stencilFrontFace.depthFailOp = ConvertStencilOp(dc.stencilzfail);

      const rex::graphics::reg::PA_SU_SC_MODE_CNTL mode{key.modeControl};
      if (dc.backface_enable) {
        desc.stencilBackFace.compareFunction =
            ConvertCompareFunction(dc.stencilfunc_bf, key.reverseZ);
        desc.stencilBackFace.failOp = ConvertStencilOp(dc.stencilfail_bf);
        desc.stencilBackFace.passOp = ConvertStencilOp(dc.stencilzpass_bf);
        desc.stencilBackFace.depthFailOp = ConvertStencilOp(dc.stencilzfail_bf);
      } else {
        desc.stencilBackFace = desc.stencilFrontFace;
      }
      (void)mode;
    }
  }

  if (key.polyOffsetScale || key.polyOffsetBias) {
    float scale = 0.0f;
    float bias = 0.0f;
    std::memcpy(&scale, &key.polyOffsetScale, sizeof(scale));
    std::memcpy(&bias, &key.polyOffsetBias, sizeof(bias));
    desc.slopeScaledDepthBias = scale;
    desc.depthBias = static_cast<i32>(bias * 16777216.0f);
  }

  {
    const rex::graphics::reg::PA_SU_SC_MODE_CNTL mode{key.modeControl};
    if (mode.cull_front && mode.cull_back) {
      static std::atomic<u32> both{0};
      if (both.fetch_add(1, std::memory_order_relaxed) == 0)
        EOT_WARN("[pipeline] a draw asks to cull both faces; culling back only");
      desc.cullMode = plume::RenderCullMode::BACK;
    } else if (mode.cull_front) {
      desc.cullMode = plume::RenderCullMode::FRONT;
    } else if (mode.cull_back) {
      desc.cullMode = plume::RenderCullMode::BACK;
    } else {
      desc.cullMode = plume::RenderCullMode::NONE;
    }
    desc.frontFace = mode.face ? plume::RenderFrontFace::CLOCKWISE
                               : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  }

  desc.specConstants = nullptr;
  desc.specConstantsCount = 0;

  return device->createGraphicsPipeline(desc);
}

}

plume::RenderPipeline *GetOrCreatePipeline(const PipelineKey &key,
                                           const InputLayout &layout) {
  g_lookups.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(g_pipeline_mutex);
    auto it = g_pipelines.find(key);
    if (it != g_pipelines.end()) {
      ++it->second.draws;
      return it->second.pipeline.get();
    }
  }

  std::unique_ptr<plume::RenderPipeline> built = BuildPipeline(key, layout);
  if (!built && g_build_failed.fetch_add(1, std::memory_order_relaxed) < 3) {
    EOT_WARN("[pso] build failed: vs=0x{:016X} ps=0x{:016X} rt={} ds={} "
             "{} elements",
             key.vertexShaderHash, key.pixelShaderHash,
             static_cast<u32>(key.renderTargetFormat),
             static_cast<u32>(key.depthFormat), layout.count);
  }

  std::lock_guard lock(g_pipeline_mutex);
  auto [it, inserted] = g_pipelines.try_emplace(key);
  ++it->second.draws;
  if (inserted) {
    it->second.pipeline = std::move(built);
    if (it->second.pipeline)
      g_built.fetch_add(1, std::memory_order_relaxed);
  }
  if (inserted && g_pipelines.size() == 1) {
    EOT_INFO("[pso] first key: vs=0x{:016X} ps=0x{:016X} rt={} ds={} topo={}",
             key.vertexShaderHash, key.pixelShaderHash,
             static_cast<u32>(key.renderTargetFormat),
             static_cast<u32>(key.depthFormat),
             static_cast<u32>(key.topology));
  }
  if (inserted) {
    const size_t n = g_pipelines.size();
    if (n == 1 || n == 10 || n == 50 || n % 100 == 0)
      LogPipelineStatsLocked();
  }
  return it->second.pipeline.get();
}

namespace {

void LogPipelineStatsLocked() {
  u32 depth_only = 0;
  for (const auto &[key, entry] : g_pipelines) {
    if (key.pixelShaderHash == 0)
      ++depth_only;
  }
  EOT_INFO("[pso] {} distinct keys from {} lookups ({} depth-only); {} "
           "undescribable; layouts {} ok / {} failed; pipelines {} built, {} failed",
           g_pipelines.size(), g_lookups.load(), depth_only,
           g_undescribable.load(), g_layout_ok.load(),
           g_layout_failed.load(), g_built.load(), g_build_failed.load());
}

}

void LogPipelineStats() {
  std::lock_guard lock(g_pipeline_mutex);
  LogPipelineStatsLocked();
}

void NotePipelineUndescribable() {
  g_undescribable.fetch_add(1, std::memory_order_relaxed);
}

}
