#include "gpu/vertex_layout.h"

#include <memory>
#include <unordered_map>

#include <xxhash.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/shaders/guest_shaders.h"

namespace eot::gpu {

namespace {

struct LayoutCache {
  std::unordered_map<u64, std::unique_ptr<InputLayout>> map;
};

LayoutCache &cache() {
  static LayoutCache c;
  return c;
}

struct DecodedElement {
  u32 stream;
  u32 offset;
  u32 type;
  u32 usage;
  u32 usageIndex;
};

bool ReadDeclaration(u32 decl_va, std::vector<DecodedElement> &out, u64 *hash) {
  const u32 count = mem::load<u32>(decl_va + obj::kDeclElementCount);
  if (count == 0 || count > 32)
    return false;
  auto *elements = mem::at<DeclElement>(decl_va + obj::kDeclElements);
  if (!elements)
    return false;
  out.clear();
  out.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    const DeclElement &e = elements[i];
    DecodedElement d;
    d.stream = e.stream;
    d.offset = e.offset;
    d.type = e.type;
    d.usage = e.usage;
    d.usageIndex = e.usageIndex;
    if (d.stream == 0xFF)
      break;
    out.push_back(d);
  }
  *hash = XXH3_64bits(elements, count * sizeof(DeclElement));
  return !out.empty();
}

void SetSwapBit(InputLayout &l, u32 usage, u32 index) {
  const u32 bit = 1u << (index & 31);
  switch (static_cast<DeclUsage>(usage)) {
  case DeclUsage::TexCoord:
    l.swappedTexcoords |= bit;
    break;
  case DeclUsage::Normal:
    l.swappedNormals |= bit;
    break;
  case DeclUsage::Binormal:
    l.swappedBinormals |= bit;
    break;
  case DeclUsage::Tangent:
    l.swappedTangents |= bit;
    break;
  case DeclUsage::BlendWeight:
    l.swappedBlendWeights |= bit;
    break;
  case DeclUsage::Position:
    l.swappedPositions |= bit;
    break;
  default:
    break;
  }
}

}

const InputLayout *GetInputLayout(VideoState &s, GuestShader &vs, u32 declaration_va) {
  (void)s;
  if (!declaration_va || vs.isPixel)
    return nullptr;
  std::vector<DecodedElement> decl;
  u64 decl_hash = 0;
  if (!ReadDeclaration(declaration_va, decl, &decl_hash)) {
    u32 n;
    if (DiagShouldLog(0x5D00 ^ declaration_va, &n))
      EOT_WARN("[layout] declaration {:#x} unreadable", declaration_va);
    return nullptr;
  }
  const u64 key = decl_hash ^ (vs.hash * 0x9E3779B97F4A7C15ull);
  auto &c = cache();
  auto it = c.map.find(key);
  if (it != c.map.end())
    return it->second.get();

  auto layout = std::make_unique<InputLayout>();
  layout->key = key;
  u32 location = 0;
  bool ok = true;
  for (const VertexInput &in : vs.inputs) {
    const DecodedElement *match = nullptr;
    for (const auto &d : decl) {
      if (d.usage == in.usage && d.usageIndex == in.usageIndex) {
        match = &d;
        break;
      }
    }
    const char *semantic = DeclUsageSemantic(in.usage);
    if (!match) {
      layout->elements.emplace_back(semantic, in.usageIndex, location++,
                                    plume::RenderFormat::R32G32B32A32_FLOAT,
                                    kSyntheticVertexSlot, 0);
      layout->needsSyntheticSlot = true;
      continue;
    }
    DeclTypeInfo t = DecodeDeclType(match->type);
    if (t.format == plume::RenderFormat::UNKNOWN) {
      EOT_WARN("[layout] vs {:016x}: {}{} decl type {:#x} (xenos fmt {}) has no host format",
               vs.hash, semantic, in.usageIndex, match->type, t.xenosFormat);
      ok = false;
      break;
    }
    if (in.usage == static_cast<u32>(DeclUsage::BlendIndices)) {
      if (t.format == plume::RenderFormat::R8G8B8A8_UNORM ||
          t.format == plume::RenderFormat::B8G8R8A8_UNORM)
        t.format = plume::RenderFormat::R8G8B8A8_UINT;
    }
    if (in.usage == static_cast<u32>(DeclUsage::TexCoord) && t.integer && t.sixteenBit) {
      t.format = t.byteSize == 4 ? plume::RenderFormat::R16G16_UINT
                                 : plume::RenderFormat::R16G16B16A16_UINT;
      layout->sintTexcoords |= 1u << in.usageIndex;
    }
    if (t.sixteenBit)
      SetSwapBit(*layout, in.usage, in.usageIndex);
    if (t.packedDec3) {
      switch (static_cast<DeclUsage>(in.usage)) {
      case DeclUsage::Normal:
        layout->packedDec3 |= 1u << in.usageIndex;
        break;
      case DeclUsage::Tangent:
        layout->packedDec3 |= 1u << (8 + in.usageIndex);
        break;
      case DeclUsage::Binormal:
        layout->packedDec3 |= 1u << (16 + in.usageIndex);
        break;
      default:
        break;
      }
      layout->anyPacked111110 = true;
    }
    if (t.packed111110)
      layout->anyPacked111110 = true;
    if (match->stream >= 16) {
      EOT_WARN("[layout] vs {:016x}: stream {} out of range", vs.hash, match->stream);
      ok = false;
      break;
    }
    layout->elements.emplace_back(semantic, in.usageIndex, location++, t.format, match->stream,
                                  match->offset);
    layout->streamMask |= 1u << match->stream;
    const u32 extent = match->offset + t.byteSize;
    if (extent > layout->streamExtent[match->stream])
      layout->streamExtent[match->stream] = extent;
  }
  if (!ok)
    return nullptr;
  if (layout->anyPacked111110)
    layout->spec |= kSpecR11G11B10Normal;
  if (layout->sintTexcoords)
    layout->spec |= kSpecSintTexcoord;
  auto *raw = layout.get();
  c.map.emplace(key, std::move(layout));
  return raw;
}

}
