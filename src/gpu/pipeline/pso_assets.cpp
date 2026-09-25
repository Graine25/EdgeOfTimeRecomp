#include "gpu/pipeline/pso_assets.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_set>
#include <vector>

#include <rex/memory/utils.h>
#include <xxhash.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/pipeline/pso_records.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/vertex_layout.h"

namespace eot::gpu {

namespace {

namespace model {
constexpr u32 kMaterials = 0x54;
constexpr u32 kMaterialCount = 0x6C;
constexpr u32 kFirstMeshRecord = 0xD4;
constexpr u32 kMeshRecordSize = 0x6C;
constexpr u32 kMaxMeshRecords = 4;
}
namespace mesh {
constexpr u32 kCategoryCounts = 0x00;
constexpr u32 kDescriptorArray = 0x08;
}
namespace descr {
constexpr u32 kMaterialIndex = 0x00;
constexpr u32 kStride = 0x08;
}
namespace material {
constexpr u32 kSize = 0x3C0;
constexpr u32 kAlphaRef = 0x170;
constexpr u32 kPasses = 0x1F8;
constexpr u32 kPassSize = 0x58;
constexpr u32 kMaxPasses = 5;
constexpr u32 kTechniques = 11;
constexpr u32 kFallbackTechnique = 3;
}
namespace node {
constexpr u32 kVertexDecl = 0x20;
constexpr u32 kVertexShader = 0x24;
constexpr u32 kPixelShader = 0x20;
}

constexpr u32 kMaxDescriptorsPerMesh = 4096;
constexpr u32 kMaxMaterials = 512;
constexpr u32 kMaxAdaptedStates = 8;

struct Guest {
  const u8 *p = nullptr;
  explicit Guest(u32 va) : p(mem::at<u8>(va)) {}
  explicit operator bool() const { return p != nullptr; }
  u32 U32(u32 off) const { return rex::memory::load_and_swap<u32>(p + off); }
  u16 U16(u32 off) const { return rex::memory::load_and_swap<u16>(p + off); }
  f32 F32(u32 off) const { return std::bit_cast<f32>(U32(off)); }
};

struct Slot {
  u64 vsHash = 0, psHash = 0;
  u32 stride = 0;
  bool strideKnown = false;
  bool alphaKnown = false;
  bool alphaTest = false;
  u32 declCount = 0;
  u8 declRaw[32 * sizeof(DeclElement)] = {};
};

struct Assets {
  std::mutex mutex;
  PsoAssetStats stats;
};

Assets &assets() {
  static Assets a;
  return a;
}

u32 PackedStride(const DeclElement *elements, u32 count) {
  u32 end = 0;
  for (u32 i = 0; i < count; ++i) {
    const u32 stream = elements[i].stream;
    if (stream == 0xFF)
      break;
    if (stream != 0)
      continue;
    const DeclTypeInfo t = DecodeDeclType(elements[i].type);
    end = std::max(end, static_cast<u32>(elements[i].offset) + t.byteSize);
  }
  return (end + 3) & ~3u;
}

GuestShader *Shader(VideoState &s, u32 object, bool pixel) {
  if (!object)
    return nullptr;
  GuestShader *g = FindGuestShader(s, object);
  return g ? g : RegisterGuestShader(s, object, pixel);
}

bool MakeSlot(VideoState &s, u32 vs_obj, u32 ps_obj, u32 decl_va, Slot &slot) {
  GuestShader *vs = Shader(s, vs_obj, false);
  GuestShader *ps = Shader(s, ps_obj, true);
  if (!vs || !vs->entry || (ps_obj && (!ps || !ps->entry)) || !decl_va)
    return false;
  const u32 count = mem::load<u32>(decl_va + obj::kDeclElementCount);
  const auto *elements = mem::at<DeclElement>(decl_va + obj::kDeclElements);
  if (!elements || count == 0 || count > 32)
    return false;
  slot.vsHash = vs->hash;
  slot.psHash = ps ? ps->hash : 0;
  slot.declCount = count;
  std::memcpy(slot.declRaw, elements, count * sizeof(DeclElement));
  CanonicalizeDeclElements(reinterpret_cast<DeclElement *>(slot.declRaw), count);
  return true;
}

void WalkMaterial(VideoState &s, const Guest &mat, const std::set<u32> *strides, std::vector<Slot> &out) {
  const bool alpha_test = mat.F32(material::kAlphaRef) != 0.0f;
  for (u32 pass = 0; pass < material::kMaxPasses; ++pass) {
    const u32 pass_off = material::kPasses + pass * material::kPassSize;
    const auto vs_node = [&](u32 t) { return mat.U32(pass_off + 4 * t); };
    const auto ps_node = [&](u32 t) { return mat.U32(pass_off + 4 * (material::kTechniques + t)); };
    for (u32 t = 0; t < material::kTechniques; ++t) {
      const u32 vs_va = vs_node(t) ? vs_node(t) : vs_node(material::kFallbackTechnique);
      const u32 ps_va = ps_node(t) ? ps_node(t) : ps_node(material::kFallbackTechnique);
      Guest vnode(vs_va);
      if (!vs_va || !vnode)
        continue;
      u32 ps_obj = 0;
      if (ps_va) {
        Guest pnode(ps_va);
        ps_obj = pnode ? pnode.U32(node::kPixelShader) : 0;
      }
      Slot slot;
      if (!MakeSlot(s, vnode.U32(node::kVertexShader), ps_obj, vnode.U32(node::kVertexDecl), slot))
        continue;
      slot.alphaKnown = true;
      slot.alphaTest = alpha_test;
      if (strides) {
        slot.strideKnown = true;
        for (const u32 stride : *strides) {
          slot.stride = stride;
          out.push_back(slot);
        }
        continue;
      }
      slot.stride = PackedStride(reinterpret_cast<const DeclElement *>(slot.declRaw), slot.declCount);
      if (slot.stride)
        out.push_back(slot);
    }
  }
}

void WalkModel(VideoState &s, u32 model_va, std::vector<Slot> &out) {
  Guest m(model_va);
  if (!m)
    return;
  const u32 material_count = m.U32(model::kMaterialCount);
  const u32 materials_va = m.U32(model::kMaterials);
  if (!materials_va || material_count == 0 || material_count > kMaxMaterials)
    return;
  std::set<std::pair<u32, u32>> refs;
  for (u32 mi = 0; mi < model::kMaxMeshRecords; ++mi) {
    const u32 rec = model::kFirstMeshRecord + mi * model::kMeshRecordSize;
    const u32 n = m.U16(rec + mesh::kCategoryCounts) + m.U16(rec + mesh::kCategoryCounts + 2) +
                  m.U16(rec + mesh::kCategoryCounts + 4);
    Guest arr(m.U32(rec + mesh::kDescriptorArray));
    if (!arr || n == 0 || n > kMaxDescriptorsPerMesh)
      continue;
    for (u32 d = 0; d < n; ++d) {
      Guest desc(arr.U32(d * 4));
      if (!desc)
        continue;
      const u32 material_index = desc.U32(descr::kMaterialIndex);
      const u32 stride = desc.U32(descr::kStride);
      if (material_index < material_count && stride != 0 && stride <= 256)
        refs.emplace(material_index, stride);
    }
  }
  for (auto it = refs.begin(); it != refs.end();) {
    const u32 material_index = it->first;
    std::set<u32> strides;
    for (; it != refs.end() && it->first == material_index; ++it)
      strides.insert(it->second);
    Guest mat(materials_va + material_index * material::kSize);
    if (mat)
      WalkMaterial(s, mat, &strides, out);
  }
}

u64 SlotKey(const Slot &slot) {
  const u64 h = XXH3_64bits(slot.declRaw, slot.declCount * sizeof(DeclElement));
  return PsoPairKey(slot.vsHash, slot.psHash) ^ (h * 31) ^ slot.stride ^ (slot.alphaTest ? 1ull << 63 : 0);
}

enum class Alpha { Keep, Off, On };

u32 Adapt(const PsoRecord &row, const Slot &slot, const InputLayout &layout, u32 spec_mask, Alpha alpha) {
  PsoRecord r = row;
  r.packageCount = 0;
  r.state.layoutKey = layout.key;
  r.state.spec = (r.state.drawnSpec & ~kSpecLayoutBits) | layout.spec;
  if (alpha != Alpha::Keep)
    r.state.spec = alpha == Alpha::On ? r.state.spec | kSpecAlphaTest : r.state.spec & ~kSpecAlphaTest;
  r.state.strides[0] = slot.stride;
  r.declCount = slot.declCount;
  std::memset(r.declRaw, 0, sizeof(r.declRaw));
  std::memcpy(r.declRaw, slot.declRaw, slot.declCount * sizeof(DeclElement));
  CanonicalizePipelineState(r.state, spec_mask, layout.streamMask);
  return PsoCacheQueue(r, PsoSource::Asset, false);
}

void QueueSlots(const std::vector<Slot> &slots) {
  const PsoList &list = PsoListGet();
  PsoAssetStats add;
  std::unordered_set<u64> seen;
  for (const Slot &slot : slots) {
    if (!seen.insert(SlotKey(slot)).second)
      continue;
    ++add.slots;
    const auto pair = list.byPair.find(PsoPairKey(slot.vsHash, slot.psHash));
    if (pair == list.byPair.end()) {
      ++add.unknown;
      continue;
    }
    const ShaderCacheEntry *entry = FindShaderCacheEntry(slot.vsHash);
    if (!entry)
      continue;
    std::vector<VertexInput> inputs;
    VertexInputsFromEntry(*entry, inputs);
    const InputLayout *layout = GetInputLayoutFromRaw(slot.vsHash, inputs, slot.declRaw, slot.declCount);
    if (!layout)
      continue;
    u32 spec_mask = entry->specConstantsMask;
    if (slot.psHash)
      if (const ShaderCacheEntry *pe = FindShaderCacheEntry(slot.psHash))
        spec_mask |= pe->specConstantsMask;
    const bool alpha_matters = slot.alphaKnown && (spec_mask & kSpecAlphaTest);
    std::vector<u32> same_layout;
    bool same_stride = false, same_alpha = false;
    for (const u32 row : pair->second) {
      const PipelineState &st = list.rows[row].state;
      if (st.layoutKey != layout->key)
        continue;
      same_layout.push_back(row);
      same_stride |= st.strides[0] == slot.stride;
      same_alpha |= ((st.drawnSpec & kSpecAlphaTest) != 0) == slot.alphaTest;
    }
    const bool new_layout = same_layout.empty() || (slot.strideKnown && !same_stride);
    const bool new_alpha = !same_layout.empty() && alpha_matters && !same_alpha;
    if (!new_layout && !new_alpha) {
      ++add.recorded;
      continue;
    }
    const std::vector<u32> &sources = same_layout.empty() ? pair->second : same_layout;
    const Alpha alpha = !alpha_matters ? Alpha::Keep : slot.alphaTest ? Alpha::On : Alpha::Off;
    u32 queued = 0;
    for (const u32 row : sources) {
      if (queued >= kMaxAdaptedStates)
        break;
      queued += Adapt(list.rows[row], slot, *layout, spec_mask, alpha) ? 1 : 0;
    }
    add.queued += queued;
    (new_layout ? add.layouts : add.alpha) += queued ? 1 : 0;
  }
  auto &a = assets();
  std::lock_guard lock(a.mutex);
  a.stats.slots += add.slots;
  a.stats.recorded += add.recorded;
  a.stats.layouts += add.layouts;
  a.stats.alpha += add.alpha;
  a.stats.unknown += add.unknown;
  a.stats.queued += add.queued;
}

bool Ready(VideoState &s) { return s.ready && s.device && !PsoListGet().rows.empty(); }

void Count(u32 PsoAssetStats::*field) {
  auto &a = assets();
  std::lock_guard lock(a.mutex);
  ++(a.stats.*field);
}

}

void PsoAssetsMaterialLoaded(u32 material_va) {
  EOT_CPU_ZONE("pso material");
  auto &s = state();
  if (!material_va || !Ready(s))
    return;
  std::vector<Slot> slots;
  Guest mat(material_va);
  if (mat)
    WalkMaterial(s, mat, nullptr, slots);
  Count(&PsoAssetStats::materials);
  QueueSlots(slots);
}

void PsoAssetsModelLoading(u32 model_va) {
  EOT_CPU_ZONE("pso model");
  auto &s = state();
  if (!model_va || !Ready(s))
    return;
  std::vector<Slot> slots;
  WalkModel(s, model_va, slots);
  Count(&PsoAssetStats::models);
  QueueSlots(slots);
}

void PsoAssetsShaderBundle(u32 bundle_va) {
  auto &s = state();
  Guest b(bundle_va);
  if (!b || !Ready(s))
    return;
  const u32 ps = b.U32(0), decl = b.U32(4), vs = b.U32(8);
  if (GuestShader *g = Shader(s, vs, false))
    g->createdByGuestCall = true;
  if (GuestShader *g = Shader(s, ps, true))
    g->createdByGuestCall = true;
  Slot slot;
  if (!MakeSlot(s, vs, ps, decl, slot))
    return;
  slot.stride = PackedStride(reinterpret_cast<const DeclElement *>(slot.declRaw), slot.declCount);
  Count(&PsoAssetStats::bundles);
  QueueSlots({slot});
}

PsoAssetStats PsoAssetsGetStats() {
  auto &a = assets();
  std::lock_guard lock(a.mutex);
  return a.stats;
}

}
