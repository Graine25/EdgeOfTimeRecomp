#include "gpu/guest/vertex_declaration.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/guest/d3d.h"
#include "gpu/guest/format.h"

#include <plume_render_interface.h>
#include <unordered_set>

namespace eot::gpu {

namespace {

std::atomic<u32> g_decls{0};
std::atomic<u32> g_decl_bad{0};

std::mutex g_decltype_mutex;
struct DeclTypeInfo {
  u32 minSize = ~0u;
  u32 maxSize = 0;
  u32 seen = 0;
};
std::unordered_map<u32, DeclTypeInfo> g_decl_types;

}

u32 DeclarationsStamped() { return g_decls.load(); }
u32 DeclarationsUnreadable() { return g_decl_bad.load(); }

void LogDeclTypeCensus() {
  std::lock_guard lock(g_decltype_mutex);
  EOT_INFO("[vtxdecl] {} distinct D3DDECLTYPE values:", g_decl_types.size());
  for (const auto &[type, info] : g_decl_types) {
    if (info.minSize == ~0u) {
      EOT_INFO("[vtxdecl]   0x{:06X} x{} (size unknown - always last)", type,
               info.seen);
    } else if (info.minSize == info.maxSize) {
      EOT_INFO("[vtxdecl]   0x{:06X} x{} -> {} bytes", type, info.seen,
               info.minSize);
    } else {
      EOT_INFO("[vtxdecl]   0x{:06X} x{} -> {}..{} bytes (AMBIGUOUS)", type,
               info.seen, info.minSize, info.maxSize);
    }
  }
}

const char *VertexUsageName(VertexUsage usage) {
  switch (usage) {
  case VertexUsage::kPosition:
    return "POSITION";
  case VertexUsage::kBlendWeight:
    return "BLENDWEIGHT";
  case VertexUsage::kBlendIndices:
    return "BLENDINDICES";
  case VertexUsage::kNormal:
    return "NORMAL";
  case VertexUsage::kPointSize:
    return "PSIZE";
  case VertexUsage::kTexCoord:
    return "TEXCOORD";
  case VertexUsage::kTangent:
    return "TANGENT";
  case VertexUsage::kBinormal:
    return "BINORMAL";
  case VertexUsage::kTessFactor:
    return "TESSFACTOR";
  case VertexUsage::kPositionT:
    return "POSITIONT";
  case VertexUsage::kColor:
    return "COLOR";
  case VertexUsage::kFog:
    return "FOG";
  case VertexUsage::kDepth:
    return "DEPTH";
  case VertexUsage::kSample:
    return "SAMPLE";
  default:
    return "?";
  }
}

bool DecodeVertexDeclaration(u32 decl_va, VertexDeclaration &out) {
  out = VertexDeclaration{};
  if (!decl_va)
    return false;
  if (mem::try_load<u32>(decl_va) != kVertexDeclCommonSignature)
    return false;
  if (mem::try_load<u32>(decl_va + 0x14) != kVertexDeclBaseFlushSignature)
    return false;

  const u32 count = mem::try_load<u32>(decl_va + kVertexDeclCountOffset);
  if (count == 0 || count > kMaxDeclElements)
    return false;
  out.maxStream = mem::try_load<u32>(decl_va + kVertexDeclMaxStreamOffset);

  for (u32 i = 0; i < count; ++i) {
    const auto *e = mem::try_at<const D3DVertexElement>(
        decl_va + kVertexDeclElementsOffset + i * kVertexDeclElementStride);
    if (!e)
      return false;
    if (VertexElementStream(*e) == kVertexDeclEndStream)
      break;
    VertexDeclElement &d = out.elements[out.count++];
    d.stream = VertexElementStream(*e);
    d.offset = VertexElementOffset(*e);
    d.declType = e->Type;
    d.usage = static_cast<VertexUsage>(VertexElementUsage(*e));
    d.usageIndex = VertexElementUsageIndex(*e);
  }
  return out.count != 0;
}

void RegisterVertexDeclaration(u32 decl_va) {
  VertexDeclaration decl;
  if (!DecodeVertexDeclaration(decl_va, decl)) {
    g_decl_bad.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  {
    std::lock_guard lock(g_decltype_mutex);
    for (u32 i = 0; i < decl.count; ++i) {
      const u32 type = decl.elements[i].declType;
      auto [it, inserted] = g_decl_types.try_emplace(type);
      auto &info = it->second;
      ++info.seen;
      if (inserted) {
        const plume::RenderFormat fmt = ConvertDeclType(type);
        if (fmt == plume::RenderFormat::UNKNOWN) {
          EOT_WARN("[vtxdecl] UNMAPPED D3DDECLTYPE 0x{:06X} (as {}{})", type,
                   VertexUsageName(decl.elements[i].usage),
                   decl.elements[i].usageIndex);
        } else {
          EOT_INFO("[vtxdecl] new D3DDECLTYPE 0x{:06X} -> fmt {} (as {}{})",
                   type, static_cast<u32>(fmt),
                   VertexUsageName(decl.elements[i].usage),
                   decl.elements[i].usageIndex);
        }
      }

      if (i + 1 < decl.count &&
          decl.elements[i + 1].stream == decl.elements[i].stream &&
          decl.elements[i + 1].offset > decl.elements[i].offset) {
        const u32 size = decl.elements[i + 1].offset - decl.elements[i].offset;
        const u32 old_min = info.minSize;
        const u32 old_max = info.maxSize;
        info.minSize = std::min(info.minSize, size);
        info.maxSize = std::max(info.maxSize, size);
        if (old_min == ~0u) {
          EOT_INFO("[vtxdecl]   0x{:06X} -> {} bytes", type, size);
        } else if (info.minSize != old_min || info.maxSize != old_max) {
          EOT_WARN("[vtxdecl]   0x{:06X} size now {}..{} - AMBIGUOUS", type,
                   info.minSize, info.maxSize);
        }
      }
    }
  }

  if (g_decls.fetch_add(1, std::memory_order_relaxed) < 4) {
    char buf[320];
    int len = 0;
    for (u32 i = 0; i < decl.count && len < int(sizeof(buf)) - 40; ++i) {
      const auto &e = decl.elements[i];
      len += snprintf(buf + len, sizeof(buf) - len, "%ss%u+%u:%s%u/t%u",
                      i ? " " : "", e.stream, e.offset,
                      VertexUsageName(e.usage), e.usageIndex, e.declType);
    }
    EOT_INFO("[vtxdecl] 0x{:08X}: {} elements, {} streams - {}", decl_va,
             decl.count, decl.maxStream + 1, buf);
  }
}

bool CurrentVertexDeclaration(u32 device_va, VertexDeclaration &out) {
  if (!device_va)
    return false;
  const u32 decl_va = mem::try_load<u32>(device_va + kDeviceVertexDeclShadow);
  return DecodeVertexDeclaration(decl_va, out);
}

}
