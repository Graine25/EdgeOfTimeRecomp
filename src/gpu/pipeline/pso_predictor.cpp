#include "gpu/pipeline/pso_predictor.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/memory/utils.h>

#include "core/logging.h"
#include "core/profiling.h"

#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/pipeline/pso_precache.h"
#include "gpu/pipeline/pso_records.h"
#include "gpu/settings.h"
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
constexpr u32 kFlags = 0x0C;
constexpr u32 kPassCount = 0x70;
constexpr u32 kBlendModeIndex = 0x106;
constexpr u32 kAlphaRef = 0x170;
constexpr u32 kDepthBias = 0x1C0;
constexpr u32 kSecondaryBias = 0x1E8;
constexpr u32 kPasses = 0x1F8;
constexpr u32 kPassSize = 0x58;
constexpr u32 kMaxPasses = 5;
constexpr u32 kTechniques = 11;
constexpr u32 kFallbackTechnique = 3;
}
namespace node {
constexpr u32 kVertexDecl = 0x20;
constexpr u32 kVertexShader = 0x24; // vertex node: D3D vertex shader object
constexpr u32 kPixelShader = 0x20;  // pixel node: D3D pixel shader object
}

constexpr u32 kMaxDescriptorsPerMesh = 4096;
constexpr u8 kBundleTechnique = 255;
constexpr u32 kMaxMaterials = 512;

struct Guest {
  const u8 *p = nullptr;
  explicit Guest(u32 va) : p(mem::at<u8>(va)) {}
  explicit operator bool() const { return p != nullptr; }
  u32 U32(u32 off) const { return rex::memory::load_and_swap<u32>(p + off); }
  u16 U16(u32 off) const { return rex::memory::load_and_swap<u16>(p + off); }
  u8 U8(u32 off) const { return p[off]; }
  f32 F32(u32 off) const { return std::bit_cast<f32>(U32(off)); }
};

u32 MaterialClass(const Guest &mat) {
  const u32 flags = mat.U32(material::kFlags);
  u32 c = mat.U8(material::kBlendModeIndex) & 3;
  if (mat.F32(material::kAlphaRef) != 0.0f)
    c |= 1u << 2;
  if (mat.F32(material::kDepthBias) != 0.0f)
    c |= 1u << 3;
  if (mat.F32(material::kSecondaryBias) != 0.0f)
    c |= 1u << 4;
  const u32 bits[] = {0, 8, 9, 11, 15, 22, 31};
  for (u32 i = 0; i < 7; ++i)
    if (flags & (1u << bits[i]))
      c |= 1u << (5 + i);
  return c;
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

struct Slot {
  u64 vsHash = 0, psHash = 0;
  u32 stride = 0;
  u32 materialFlags = 0;
  u32 materialClass = 0;
  u8 technique = 0, pass = 0;
  u32 declCount = 0;
  u8 declRaw[32 * sizeof(DeclElement)] = {};
};

struct WalkDiag {
  u32 nullVs = 0, nullPs = 0;
};

struct Predictor {
  std::mutex mutex;
  PsoPredictorStats stats;
  std::unordered_set<u32> noTemplateLogged;
  std::set<std::tuple<u64, u64, u8, u8, u32>> pairsWritten;
  std::string pairsPath;
  bool pairsHeader = false;
  std::string predictedPath;
  bool predictedHeader = false;
  std::unordered_map<u32, std::pair<u32, u8>> nodeObjects;
};

Predictor &predictor() {
  static Predictor p;
  return p;
}

void WritePairsLocked(Predictor &p, const std::vector<Slot> &slots) {
  const std::string dir = Settings::PsoDir();
  if (dir.empty() || !Settings::PsoCapture())
    return;
  std::string rows;
  for (const Slot &s : slots) {
    if (!p.pairsWritten.emplace(s.vsHash, s.psHash, s.technique, s.pass, s.materialClass).second)
      continue;
    rows += std::format("{:016x},{:016x},{},{},{},{:x},{:x},{}\n", s.vsHash, s.psHash,
                        s.technique, s.pass, s.stride, s.materialFlags, s.materialClass,
                        Settings::PsoTag().empty() ? "-" : Settings::PsoTag());
  }
  if (rows.empty())
    return;
  if (p.pairsPath.empty()) {
    const std::string tag = Settings::PsoTag();
    p.pairsPath =
        dir + "/pso_pairs_" + (tag.empty() ? "" : tag + "_") + PsoSessionStamp() + ".csv";
  }
  FILE *f = std::fopen(p.pairsPath.c_str(), p.pairsHeader ? "ab" : "wb");
  if (!f)
    return;
  if (!p.pairsHeader) {
    const std::string h = std::format("# eot-pso-pairs v{}\nvsHash,psHash,technique,pass,stride,"
                                      "materialFlags,class,session\n",
                                      kPsoCsvVersion);
    std::fwrite(h.data(), 1, h.size(), f);
    p.pairsHeader = true;
  }
  std::fwrite(rows.data(), 1, rows.size(), f);
  std::fclose(f);
}

void WritePredictedLocked(Predictor &p,
                          const std::vector<std::pair<PsoRecord, std::tuple<u8, u8, u32>>> &recs) {
  const std::string dir = Settings::PsoDir();
  if (dir.empty() || !Settings::PsoCapture() || recs.empty())
    return;
  if (p.predictedPath.empty()) {
    const std::string tag = Settings::PsoTag();
    p.predictedPath =
        dir + "/pso_predicted_" + (tag.empty() ? "" : tag + "_") + PsoSessionStamp() + ".csv";
  }
  FILE *f = std::fopen(p.predictedPath.c_str(), p.predictedHeader ? "ab" : "wb");
  if (!f)
    return;
  if (!p.predictedHeader) {
    const std::string h = PsoCsvHeader() + "\n";
    std::fwrite(h.data(), 1, h.size(), f);
    p.predictedHeader = true;
  }
  for (const auto &[r, slot] : recs) {
    const std::string row =
        PsoRecordToCsv(r, std::format("t{}p{}c{:x}", std::get<0>(slot), std::get<1>(slot),
                                      std::get<2>(slot))) +
        "\n";
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

void WalkMaterial(VideoState &s, const Guest &mat, const std::set<u32> *strides,
                  std::vector<Slot> &out, WalkDiag &diag) {
  const u32 mat_va = static_cast<u32>(reinterpret_cast<uintptr_t>(mat.p) & 0xFFFFFFFFu);
  const u32 flags = mat.U32(material::kFlags);
  const u32 mclass = MaterialClass(mat);
  for (u32 pass = 0; pass < material::kMaxPasses; ++pass) {
    const u32 pass_off = material::kPasses + pass * material::kPassSize;
    auto vs_node = [&](u32 t) { return mat.U32(pass_off + 4 * t); };
    auto ps_node = [&](u32 t) { return mat.U32(pass_off + 4 * (material::kTechniques + t)); };
    for (u32 t = 0; t < material::kTechniques; ++t) {
      u32 vs_va = vs_node(t), ps_va = ps_node(t);
      if (!vs_va)
        vs_va = vs_node(material::kFallbackTechnique);
      if (!ps_va)
        ps_va = ps_node(material::kFallbackTechnique);
      if (!vs_va)
        continue;
      Guest vnode(vs_va);
      if (!vnode)
        continue;
      const u32 vs_obj = vnode.U32(node::kVertexShader);
      const u32 decl_va = vnode.U32(node::kVertexDecl);
      if (!vs_obj || !decl_va) {
        ++diag.nullVs;
        continue;
      }
      u32 ps_obj = 0;
      if (ps_va) {
        Guest pnode(ps_va);
        if (pnode)
          ps_obj = pnode.U32(node::kPixelShader);
        if (!ps_obj)
          ++diag.nullPs;
      }
      {
        auto &p = predictor();
        std::lock_guard lock(p.mutex);
        p.nodeObjects.emplace(vs_obj, std::make_pair(mat_va, static_cast<u8>(t)));
        if (ps_obj)
          p.nodeObjects.emplace(ps_obj, std::make_pair(mat_va, static_cast<u8>(t)));
      }
      GuestShader *vs = FindGuestShader(s, vs_obj);
      if (!vs)
        vs = RegisterGuestShader(s, vs_obj, false);
      if (!vs || !vs->entry)
        continue;
      GuestShader *ps = nullptr;
      if (ps_obj) {
        ps = FindGuestShader(s, ps_obj);
        if (!ps)
          ps = RegisterGuestShader(s, ps_obj, true);
        if (!ps || !ps->entry)
          continue;
      }
      const u32 count = mem::load<u32>(decl_va + obj::kDeclElementCount);
      const auto *elements = mem::at<DeclElement>(decl_va + obj::kDeclElements);
      if (!elements || count == 0 || count > 32)
        continue;
      Slot slot;
      slot.vsHash = vs->hash;
      slot.psHash = ps ? ps->hash : 0;
      slot.materialFlags = flags;
      slot.materialClass = mclass;
      slot.technique = static_cast<u8>(t);
      slot.pass = static_cast<u8>(pass);
      slot.declCount = count;
      std::memcpy(slot.declRaw, elements, count * sizeof(DeclElement));
      CanonicalizeDeclElements(reinterpret_cast<DeclElement *>(slot.declRaw), count);
      if (strides) {
        for (u32 st : *strides) {
          slot.stride = st;
          out.push_back(slot);
        }
      } else {
        slot.stride = PackedStride(reinterpret_cast<const DeclElement *>(slot.declRaw), count);
        if (slot.stride)
          out.push_back(slot);
      }
    }
  }
}

struct ModelDiag {
  u32 materials = 0, refs = 0;
  WalkDiag walk;
};

void SnapshotModel(VideoState &s, u32 model_va, std::vector<Slot> &out, ModelDiag &diag) {
  Guest m(model_va);
  if (!m)
    return;
  const u32 material_count = m.U32(model::kMaterialCount);
  const u32 materials_va = m.U32(model::kMaterials);
  if (!materials_va || material_count == 0 || material_count > kMaxMaterials)
    return;
  std::set<std::pair<u32, u32>> refs;
  for (u32 mi = 0; mi < model::kMaxMeshRecords; ++mi) {
    const u32 rec_off = model::kFirstMeshRecord + mi * model::kMeshRecordSize;
    const u32 n = m.U16(rec_off + mesh::kCategoryCounts) +
                  m.U16(rec_off + mesh::kCategoryCounts + 2) +
                  m.U16(rec_off + mesh::kCategoryCounts + 4);
    const u32 array_va = m.U32(rec_off + mesh::kDescriptorArray);
    if (!array_va || n == 0 || n > kMaxDescriptorsPerMesh)
      continue;
    Guest arr(array_va);
    if (!arr)
      continue;
    for (u32 d = 0; d < n; ++d) {
      Guest desc(arr.U32(d * 4));
      if (!desc)
        continue;
      const u32 material_index = desc.U32(descr::kMaterialIndex);
      const u32 stride = desc.U32(descr::kStride);
      if (material_index >= material_count || stride == 0 || stride > 256)
        continue;
      refs.emplace(material_index, stride);
    }
  }
  if (Settings::PsoPredictAll()) {
    std::set<u32> strides;
    for (const auto &[mi, st] : refs)
      strides.insert(st);
    refs.clear();
    for (u32 mi = 0; mi < material_count; ++mi)
      for (u32 st : strides)
        refs.emplace(mi, st);
  }
  diag.materials = material_count;
  diag.refs = static_cast<u32>(refs.size());
  for (auto it = refs.begin(); it != refs.end();) {
    const u32 material_index = it->first;
    std::set<u32> strides;
    for (; it != refs.end() && it->first == material_index; ++it)
      strides.insert(it->second);
    Guest mat(materials_va + material_index * material::kSize);
    if (mat)
      WalkMaterial(s, mat, &strides, out, diag.walk);
  }
}

u32 EnqueueSlots(const std::vector<Slot> &slots, bool priority, u32 *no_template,
                 std::vector<std::pair<PsoRecord, std::tuple<u8, u8, u32>>> &predicted_rows) {
  auto &p = predictor();
  const auto &templates = CompiledInTemplates();
  const TokenPtr token = PsoPrecacheCurrentToken();
  u32 queued = 0;
  std::unordered_set<u64> seen;
  for (const Slot &slot : slots) {
    const ShaderCacheEntry *entry = FindShaderCacheEntry(slot.vsHash);
    if (!entry)
      continue;
    std::vector<VertexInput> inputs;
    VertexInputsFromEntry(*entry, inputs);
    const InputLayout *layout =
        GetInputLayoutFromRaw(slot.vsHash, inputs, slot.declRaw, slot.declCount);
    if (!layout)
      continue;
    bool any = false;
    static const int min_level = 2 - std::clamp(Settings::PsoPredictFallback(), 0, 2);
    for (int level = 2; level >= min_level && !any; --level) {
      for (const PsoTemplate &t : templates) {
        if (t.technique != slot.technique)
          continue;
        if (level >= 1 && t.materialClass != slot.materialClass)
          continue;
        if (level == 2 && t.pass != slot.pass)
          continue;
        any = true;
        PsoRecord r{};
        r.state = t.state;
        r.state.vsHash = slot.vsHash;
        r.state.psHash = slot.psHash;
        r.state.layoutKey = layout->key;
        r.state.strides[0] = slot.stride;
        r.state.spec = (t.state.spec & ~kSpecLayoutBits) | layout->spec;
        r.declCount = slot.declCount;
        std::memcpy(r.declRaw, slot.declRaw, slot.declCount * sizeof(DeclElement));
        if (!seen.insert(HashPipelineState(r.state)).second)
          continue;
        if (PsoPrecacheEnqueue(r, PsoSource::Predicted, priority, token)) {
          ++queued;
          predicted_rows.emplace_back(
              r, std::make_tuple(slot.technique, slot.pass, slot.materialClass));
        }
      }
    }
    if (!any) {
      ++*no_template;
      std::lock_guard lock(p.mutex);
      if (p.noTemplateLogged.insert(slot.technique).second)
        EOT_INFO("[pso] predictor: no template for technique {} yet (pass {}); capture more",
                 slot.technique, slot.pass);
    }
  }
  return queued;
}

}

u32 PredictModelLoad(u32 model_va) {
  EOT_CPU_ZONE("predict model load");
  auto &s = state();
  if (!Settings::PsoPredict() || !s.ready || !s.device || !model_va)
    return 0;
  std::vector<Slot> slots;
  ModelDiag diag;
  {
    std::lock_guard lock(s.mutex);
    SnapshotModel(s, model_va, slots, diag);
  }
  u32 no_template = 0;
  std::vector<std::pair<PsoRecord, std::tuple<u8, u8, u32>>> predicted_rows;
  const u32 queued = EnqueueSlots(slots, true, &no_template, predicted_rows);
  auto &p = predictor();
  {
    std::lock_guard lock(p.mutex);
    p.stats.models++;
    p.stats.slots += static_cast<u32>(slots.size());
    p.stats.queued += queued;
    p.stats.noTemplate += no_template;
    WritePairsLocked(p, slots);
    WritePredictedLocked(p, predicted_rows);
  }
  EOT_DEBUG("[pso] predictor: model {:#x}: {} materials, {} (material,stride) refs, {} slots -> {} "
            "queued ({} without template); unresolved shader objects: {} vs, {} ps",
            model_va, diag.materials, diag.refs, slots.size(), queued, no_template,
            diag.walk.nullVs, diag.walk.nullPs);
  return queued;
}

u32 PredictMaterialLoad(u32 material_va) {
  EOT_CPU_ZONE("predict material load");
  auto &s = state();
  if (!Settings::PsoPredict() || !s.ready || !s.device || !material_va)
    return 0;
  std::vector<Slot> slots;
  WalkDiag diag;
  {
    std::lock_guard lock(s.mutex);
    Guest mat(material_va);
    if (mat)
      WalkMaterial(s, mat, nullptr, slots, diag);
  }
  u32 no_template = 0;
  std::vector<std::pair<PsoRecord, std::tuple<u8, u8, u32>>> predicted_rows;
  const u32 queued = EnqueueSlots(slots, false, &no_template, predicted_rows);
  auto &p = predictor();
  {
    std::lock_guard lock(p.mutex);
    p.stats.materials++;
    p.stats.slots += static_cast<u32>(slots.size());
    p.stats.queued += queued;
    p.stats.noTemplate += no_template;
    WritePairsLocked(p, slots);
    WritePredictedLocked(p, predicted_rows);
  }
  return queued;
}

void PredictorNoteShaderBundle(u32 bundle_va) {
  Guest b(bundle_va);
  if (!b)
    return;
  const u32 ps = b.U32(0), decl = b.U32(4), vs = b.U32(8);
  auto &s = state();
  std::lock_guard lock(s.mutex);
  GuestShader *gvs = vs ? FindGuestShader(s, vs) : nullptr;
  if (!gvs && vs)
    gvs = RegisterGuestShader(s, vs, false);
  GuestShader *gps = ps ? FindGuestShader(s, ps) : nullptr;
  if (!gps && ps)
    gps = RegisterGuestShader(s, ps, true);
  if (gvs)
    gvs->createdByGuestCall = true;
  if (gps)
    gps->createdByGuestCall = true;
  if (!gvs || !gvs->entry || (ps && (!gps || !gps->entry)) || !decl)
    return;
  const u32 count = mem::load<u32>(decl + obj::kDeclElementCount);
  const auto *elements = mem::at<DeclElement>(decl + obj::kDeclElements);
  if (!elements || count == 0 || count > 32)
    return;
  Slot slot;
  slot.vsHash = gvs->hash;
  slot.psHash = gps ? gps->hash : 0;
  slot.technique = kBundleTechnique;
  slot.declCount = count;
  std::memcpy(slot.declRaw, elements, count * sizeof(DeclElement));
  CanonicalizeDeclElements(reinterpret_cast<DeclElement *>(slot.declRaw), count);
  slot.stride = PackedStride(reinterpret_cast<const DeclElement *>(slot.declRaw), count);
  std::vector<Slot> slots{slot};
  auto &p = predictor();
  {
    std::lock_guard plock(p.mutex);
    p.nodeObjects.emplace(vs, std::make_pair(bundle_va, kBundleTechnique));
    if (ps)
      p.nodeObjects.emplace(ps, std::make_pair(bundle_va, kBundleTechnique));
  }
  s.mutex.unlock();
  u32 no_template = 0;
  std::vector<std::pair<PsoRecord, std::tuple<u8, u8, u32>>> predicted_rows;
  const u32 queued = EnqueueSlots(slots, true, &no_template, predicted_rows);
  {
    std::lock_guard plock(p.mutex);
    p.stats.slots++;
    p.stats.queued += queued;
    p.stats.noTemplate += no_template;
    WritePairsLocked(p, slots);
    WritePredictedLocked(p, predicted_rows);
  }
  s.mutex.lock();
}

std::string PsoPredictorDescribeObject(u32 shader_object_va) {
  auto &p = predictor();
  std::lock_guard lock(p.mutex);
  auto it = p.nodeObjects.find(shader_object_va);
  if (it == p.nodeObjects.end())
    return "none";
  return std::format("mat{:x}:t{}", it->second.first, it->second.second);
}

PsoPredictorStats PsoPredictorGetStats() {
  auto &p = predictor();
  std::lock_guard lock(p.mutex);
  return p.stats;
}

}
