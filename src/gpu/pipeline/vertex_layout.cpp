#include "gpu/pipeline/vertex_layout.h"

#include <rex/graphics/xenos.h>

#include "gpu/guest/vfetch_microcode.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/guest/d3d.h"
#include "gpu/guest/format.h"
#include "gpu/shaders/shader_cache.h"

namespace eot::gpu {

namespace {

namespace xenos = rex::graphics::xenos;

constexpr u32 kUsageMask = 0xF;
constexpr u32 kUsageIndexShift = 4;
constexpr u32 kUsageIndexMask = 0xF;
constexpr u32 kMiniFetchShift = 8;
constexpr u32 kInstrAddressMask = 0xFFFF;
constexpr u32 kParentAddressShift = 16;

std::atomic<u32> g_decoded{0};
std::atomic<u32> g_no_records{0};
std::atomic<u32> g_truncated{0};
std::atomic<u32> g_join_missing{0};
std::atomic<u32> g_join_unmapped{0};
std::atomic<u32> g_join_empty{0};

std::mutex g_shape_mutex;
std::unordered_set<u64> g_shapes;

u64 ShapeOf(const VertexLayout &layout) {
  u64 h = 0xcbf29ce484222325ull;
  for (u32 i = 0; i < layout.count; ++i) {
    const auto &f = layout.fetches[i];
    if (f.miniFetch)
      continue;
    h = (h ^ (static_cast<u32>(f.usage) | (f.usageIndex << 8))) *
        0x100000001b3ull;
  }
  return h;
}

}

const char *VertexUsageSemantic(VertexUsage usage) {
  return VertexUsageName(usage);
}

bool DecodeVertexLayout(const GuestShader *vs, VertexLayout &out) {
  out = VertexLayout{};
  const ShaderCacheEntry *entry = vs ? vs->shaderCacheEntry : nullptr;
  if (!entry || entry->vertex_layout_count == 0) {
    g_no_records.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  u32 count = entry->vertex_layout_count;
  if (count > kMaxVertexFetches) {
    count = kMaxVertexFetches;
    out.truncated = true;
    if (g_truncated.fetch_add(1, std::memory_order_relaxed) == 0) {
      EOT_WARN("[vtxlayout] shader 0x{:016X} declares {} fetches, capped at {}",
               vs->hash, entry->vertex_layout_count, kMaxVertexFetches);
    }
  }

  const u32 *records = g_shaderVertexLayouts + entry->vertex_layout_offset;
  for (u32 i = 0; i < count; ++i) {
    const u32 w0 = records[i * 2];
    const u32 w1 = records[i * 2 + 1];
    VertexFetch &f = out.fetches[i];
    f.usage = static_cast<VertexUsage>(w0 & kUsageMask);
    f.usageIndex = (w0 >> kUsageIndexShift) & kUsageIndexMask;
    f.miniFetch = ((w0 >> kMiniFetchShift) & 1) != 0;
    f.instructionAddress = w1 & kInstrAddressMask;
    f.parentAddress = (w1 >> kParentAddressShift) & kInstrAddressMask;
  }
  out.count = count;

  const u64 shape = ShapeOf(out);
  bool first_shape = false;
  {
    std::lock_guard lock(g_shape_mutex);
    first_shape = g_shapes.insert(shape).second;
  }
  const u32 n = g_decoded.fetch_add(1, std::memory_order_relaxed);
  if (n == 0 || (first_shape && g_shapes.size() <= 8)) {
    char buf[256];
    int len = 0;
    for (u32 i = 0; i < out.count && len < int(sizeof(buf)) - 24; ++i) {
      len += snprintf(buf + len, sizeof(buf) - len, "%s%s%u%s",
                      i ? " " : "", VertexUsageName(out.fetches[i].usage),
                      out.fetches[i].usageIndex,
                      out.fetches[i].miniFetch ? "(mini)" : "");
    }
    EOT_INFO("[vtxlayout] shape {}: {} fetches - {}", g_shapes.size(),
             out.count, buf);
  }
  return true;
}

u32 VertexFormatSize(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R8_UNORM:
    return 1;
  case plume::RenderFormat::R8G8_UNORM:
  case plume::RenderFormat::R16_UNORM:
  case plume::RenderFormat::R16_FLOAT:
    return 2;
  case plume::RenderFormat::R8G8B8A8_UINT:
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16_SINT:
  case plume::RenderFormat::R16G16_SNORM:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R32_UINT:
  case plume::RenderFormat::R32_FLOAT:
    return 4;
  case plume::RenderFormat::R16G16B16A16_SINT:
  case plume::RenderFormat::R16G16B16A16_SNORM:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R32G32_FLOAT:
    return 8;
  case plume::RenderFormat::R32G32B32_FLOAT:
    return 12;
  case plume::RenderFormat::R32G32B32A32_FLOAT:
    return 16;
  default:
    return 0;
  }
}

namespace {

plume::RenderFormat HostFormatOf(u32 xenos_format, bool normalized,
                                 bool is_signed) {
  using VF = xenos::VertexFormat;
  using RF = plume::RenderFormat;
  switch (static_cast<VF>(xenos_format)) {
  case VF::k_8_8_8_8:
    return normalized ? RF::R8G8B8A8_UNORM : RF::R8G8B8A8_UINT;
  case VF::k_2_10_10_10:
  case VF::k_10_11_11:
  case VF::k_11_11_10:
    return RF::R32_UINT;
  case VF::k_16_16:
    if (normalized)
      return is_signed ? RF::R16G16_SNORM : RF::R16G16_UNORM;
    return RF::R16G16_SINT;
  case VF::k_16_16_16_16:
    if (normalized)
      return is_signed ? RF::R16G16B16A16_SNORM : RF::R16G16B16A16_UNORM;
    return RF::R16G16B16A16_SINT;
  case VF::k_16_16_FLOAT:
    return RF::R16G16_FLOAT;
  case VF::k_16_16_16_16_FLOAT:
    return RF::R16G16B16A16_FLOAT;
  case VF::k_32:
  case VF::k_32_FLOAT:
    return RF::R32_FLOAT;
  case VF::k_32_32:
  case VF::k_32_32_FLOAT:
    return RF::R32G32_FLOAT;
  case VF::k_32_32_32_FLOAT:
    return RF::R32G32B32_FLOAT;
  case VF::k_32_32_32_32:
  case VF::k_32_32_32_32_FLOAT:
    return RF::R32G32B32A32_FLOAT;
  default:
    return RF::UNKNOWN;
  }
}

}

bool BuildInputLayoutFromMicrocode(const GuestShader &shader,
                                   const VertexLayout &fetches,
                                   u32 buffer_stride, InputLayout &out) {
  FetchMicrocode decoded[kMaxVertexFetches];
  u32 microcode_stride = 0;
  if (!DecodeLayoutFromMicrocode(shader, fetches, decoded, microcode_stride))
    return false;

  if (!buffer_stride)
    buffer_stride = microcode_stride;
  if (!buffer_stride)
    return false;

  InputLayout built;
  for (u32 i = 0; i < fetches.count; ++i) {
    const plume::RenderFormat format = HostFormatOf(
        decoded[i].format, decoded[i].normalized, decoded[i].signedValue);
    if (format == plume::RenderFormat::UNKNOWN)
      return false;

    const u32 size = VertexFormatSize(format);
    if (!size || decoded[i].offset + size > buffer_stride)
      return false;

    InputElement &e = built.elements[built.count++];
    e.usage = fetches.fetches[i].usage;
    e.usageIndex = fetches.fetches[i].usageIndex;
    e.stream = 0;
    e.offset = decoded[i].offset;
    e.format = format;
    built.packedNormal |= format == plume::RenderFormat::R32_UINT;
  }

  out = built;
  return true;
}

bool BuildInputLayout(const VertexLayout &fetches, const VertexDeclaration &decl,
                      InputLayout &out) {
  out = InputLayout{};
  for (u32 i = 0; i < fetches.count; ++i) {
    const VertexFetch &f = fetches.fetches[i];
    if (f.miniFetch)
      continue;

    const VertexDeclElement *match = nullptr;
    for (u32 j = 0; j < decl.count; ++j) {
      if (decl.elements[j].usage == f.usage &&
          decl.elements[j].usageIndex == f.usageIndex) {
        match = &decl.elements[j];
        break;
      }
    }
    if (!match) {
      out.incomplete = true;
      if (g_join_missing.fetch_add(1, std::memory_order_relaxed) < 3) {
        char have[256];
        int len = 0;
        for (u32 j = 0; j < decl.count && len < int(sizeof(have)) - 24; ++j) {
          len += snprintf(have + len, sizeof(have) - len, "%s%s%u", j ? " " : "",
                          VertexUsageName(decl.elements[j].usage),
                          decl.elements[j].usageIndex);
        }
        EOT_WARN("[vtxlayout] shader wants {}{} - declaration has: {}",
                 VertexUsageName(f.usage), f.usageIndex, have);
      }
      continue;
    }
    const plume::RenderFormat fmt = ConvertDeclType(match->declType);
    if (fmt == plume::RenderFormat::UNKNOWN) {
      out.incomplete = true;
      if (g_join_unmapped.fetch_add(1, std::memory_order_relaxed) < 4) {
        EOT_WARN("[vtxlayout] unmapped D3DDECLTYPE 0x{:06X} for {}{} "
                 "(stream {} offset {})",
                 match->declType, VertexUsageName(f.usage), f.usageIndex,
                 match->stream, match->offset);
      }
      continue;
    }
    if (fmt == plume::RenderFormat::R32_UINT &&
        (f.usage == VertexUsage::kNormal || f.usage == VertexUsage::kTangent ||
         f.usage == VertexUsage::kBinormal))
      out.packedNormal = true;

    InputElement &e = out.elements[out.count++];
    e.usage = f.usage;
    e.usageIndex = f.usageIndex;
    e.stream = match->stream;
    e.offset = match->offset;
    e.format = fmt;
  }
  if (out.count == 0 && g_join_empty.fetch_add(1, std::memory_order_relaxed) < 3) {
    u32 mini = 0;
    for (u32 i = 0; i < fetches.count; ++i)
      mini += fetches.fetches[i].miniFetch ? 1 : 0;
    EOT_WARN("[vtxlayout] join produced no elements: {} fetches, {} of them "
             "mini, declaration has {} elements",
             fetches.count, mini, decl.count);
  }
  return out.count != 0 && !out.incomplete;
}

void LogVertexLayoutStats() {
  LogDeclTypeCensus();
  std::lock_guard lock(g_shape_mutex);
  EOT_INFO("[vtxlayout] {} decoded into {} distinct shapes; {} shaders had no "
           "records, {} truncated; {} declarations stamped, {} unreadable",
           g_decoded.load(), g_shapes.size(), g_no_records.load(),
           g_truncated.load(), DeclarationsStamped(),
           DeclarationsUnreadable());
}

}
