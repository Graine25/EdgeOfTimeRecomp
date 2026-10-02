#include "gpu/pipeline/pso_set.h"

#include <charconv>
#include <cstring>
#include <string_view>

#include "core/logging.h"
#include "gpu/format.h"
#include "gpu/settings.h"
#include "gpu/shaders/guest_shaders.h"
#include "gpu/vertex_layout.h"

namespace eot::gpu {

namespace {

const char *const kSetLines[] = {
#include "gpu/pipeline/cache/eot_pso.inc"
    nullptr};

std::vector<std::string_view> Fields(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  size_t start = 0;
  for (;;) {
    const size_t p = s.find(sep, start);
    if (p == std::string_view::npos) {
      out.push_back(s.substr(start));
      return out;
    }
    out.push_back(s.substr(start, p - start));
    start = p + 1;
  }
}

template <typename T> bool Num(std::string_view s, T *v, int base = 10) {
  if (s.empty())
    return false;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), *v, base);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool Float(std::string_view s, f32 *v) {
  if (s.empty())
    return false;
  const std::string tmp(s);
  char *end = nullptr;
  *v = std::strtof(tmp.c_str(), &end);
  return end && *end == '\0';
}

bool Hex(std::string_view s, std::vector<u8> *out) {
  if (s.size() % 2)
    return false;
  out->resize(s.size() / 2);
  for (size_t i = 0; i < s.size(); i += 2) {
    u32 b;
    if (!Num(s.substr(i, 2), &b, 16))
      return false;
    (*out)[i / 2] = static_cast<u8>(b);
  }
  return true;
}

PsoSet Parse() {
  PsoSet set;
  PsoCsvLayout layout{};
  bool have_layout = false;
  std::unordered_map<std::string_view, u16> stride_ids;
  for (const char *const *p = kSetLines; *p; ++p) {
    const std::string_view line(*p);
    const auto f = Fields(line, '\t');
    const std::string_view kind = f[0];
    bool ok = true;
    if (kind == "header") {
      have_layout = f.size() == 2 && PsoCsvParseHeader(f[1], &layout);
      ok = have_layout;
    } else if (kind == "core") {
      PsoRecord r;
      ok = have_layout && f.size() == 3 && PsoRecordFromCsv(layout, f[2], &r);
      set.cores.push_back(ok ? r.state : PipelineState{});
    } else if (kind == "vcore") {
      u32 plain, vel;
      ok = f.size() == 3 && Num(f[1], &plain) && Num(f[2], &vel) && plain < set.cores.size() &&
           vel < set.cores.size();
      if (ok) {
        set.velocityOf.resize(set.cores.size(), -1);
        set.velocityOf[plain] = static_cast<i32>(vel);
      }
    } else if (kind == "vkey") {
      PsoSetKey k;
      ok = f.size() == 4 && (f[1] == "v" || f[1] == "p") && Num(f[2], &k.key, 16);
      if (ok) {
        k.pixel = f[1] == "p";
        for (const auto h : Fields(f[3], '|')) {
          u64 v;
          if (Num(h, &v, 16))
            k.hashes.push_back(v);
        }
        const u32 index = static_cast<u32>(set.keys.size());
        (k.pixel ? set.psKeyIndex : set.vsKeyIndex)[k.key] = index;
        set.keys.push_back(std::move(k));
        set.keyRows.emplace_back();
      }
    } else if (kind == "decl") {
      std::vector<u8> bytes;
      ok = f.size() == 3 && Hex(f[2], &bytes) && bytes.size() % sizeof(DeclElement) == 0 &&
           bytes.size() <= sizeof(PsoRecord::declRaw);
      set.decls.push_back(std::move(bytes));
    } else if (kind == "pkg") {
      u32 id;
      ok = f.size() == 5 && Num(f[1], &id, 16) && id < 0x1000;
      if (ok) {
        PsoSetPackage &pk = set.packages[static_cast<u16>(id)];
        pk.name = std::string(f[2]);
        pk.level = f[4] == "1";
        if (!f[3].empty())
          for (const auto parent : Fields(f[3], '|')) {
            u32 pid;
            if (Num(parent, &pid, 16))
              pk.parents.push_back(static_cast<u16>(pid));
          }
      }
    } else if (kind == "shadow") {
      u32 id;
      PsoShadowValue v;
      ok = f.size() == 4 && Num(f[1], &id, 16) && Num(f[2], &v.depthBias) && Float(f[3], &v.slope);
      if (ok)
        set.packages[static_cast<u16>(id)].shadow.push_back(v);
    } else if (kind == "row") {
      PsoSetRow row;
      u32 decl = 0, core = 0, bias = 0, msaa = 0, vk = 0, pk = 0;
      ok = f.size() == 16 && Num(f[2], &row.vsHash, 16) && Num(f[3], &row.psHash, 16) &&
           Num(f[4], &decl) && Num(f[6], &core) && Num(f[7], &bias) && bias <= 3 &&
           Num(f[12], &row.spec, 16) && Num(f[13], &msaa) && msaa <= 3 &&
           decl < set.decls.size() && core < set.cores.size();
      row.msaa = static_cast<u8>(msaa);
      if (ok && !f[14].empty()) {
        ok = Num(f[14], &vk) && vk < set.keys.size();
        row.vk = static_cast<i32>(vk);
      }
      if (ok && !f[15].empty()) {
        ok = Num(f[15], &pk) && pk < set.keys.size();
        row.pk = static_cast<i32>(pk);
      }
      if (ok) {
        row.decl = static_cast<u16>(decl);
        row.core = static_cast<u16>(core);
        row.bias = static_cast<PsoBias>(bias);
        row.derived = f[9] == "d";
        auto [it, fresh] = stride_ids.try_emplace(f[5], static_cast<u16>(set.strides.size()));
        if (fresh) {
          std::array<u32, 16> st{};
          const auto parts = Fields(f[5], '|');
          for (size_t i = 0; i < parts.size() && i < 16; ++i)
            ok &= Num(parts[i], &st[i]);
          set.strides.push_back(st);
        }
        row.strides = it->second;
        if (row.bias == PsoBias::Material) {
          ok &= Float(f[8], &row.m);
        } else if (row.bias == PsoBias::Literal) {
          const auto bs = Fields(f[8], '/');
          PsoShadowValue v;
          ok &= bs.size() == 2 && Num(bs[0], &v.depthBias) && Float(bs[1], &v.slope);
          row.literal = static_cast<u32>(set.literals.size());
          set.literals.push_back(v);
        }
      }
      if (ok) {
        const u32 index = static_cast<u32>(set.rows.size());
        set.rows.push_back(row);
        (row.derived ? set.derived : set.captured)++;
        auto &owners = set.rowOwners.emplace_back();
        for (const auto owner : Fields(f[1], '|')) {
          u32 id;
          if (!Num(owner, &id, 16) || id >= 0x1000)
            continue;
          owners.push_back(static_cast<u16>(id));
          if (id == 0)
            set.boot.push_back(index);
          else
            set.packages[static_cast<u16>(id)].rows.push_back(index);
        }
        if (row.vk >= 0)
          set.keyRows[row.vk].push_back(index);
        if (row.pk >= 0)
          set.keyRows[row.pk].push_back(index);
      }
    }
    if (!ok)
      ++set.bad;
  }
  set.velocityOf.resize(set.cores.size(), -1);
  for (auto &[id, pk] : set.packages)
    for (const u16 parent : pk.parents) {
      auto it = set.packages.find(parent);
      if (it != set.packages.end())
        it->second.children.push_back(id);
    }
  if (set.bad || !have_layout)
    EOT_WARN("[pso] {} line(s) of the compiled-in set did not parse; regenerate cache/eot_pso.inc "
             "with tools/pso/pso_derive.py",
             set.bad);
  return set;
}

}

const PsoSet &CompiledInSet() {
  static const PsoSet set = Parse();
  return set;
}

bool PsoSetRecord(const PsoSet &set, const PsoSetRow &row, const PsoShadowValue *shadow,
                  PsoRecord *out, i32 core, const PsoHashPair *hashes) {
  PsoRecord r{};
  r.state = set.cores[core >= 0 ? static_cast<u32>(core) : row.core];
  r.msaa = row.msaa;
  PipelineState &s = r.state;
  s.vsHash = hashes ? hashes->vs : row.vsHash;
  s.psHash = hashes ? hashes->ps : row.psHash;
  s.spec = row.spec == kPsoSpecEither ? (hashes ? hashes->spec : 0) : row.spec;
  const auto &st = set.strides[row.strides];
  for (u32 i = 0; i < 16; ++i)
    s.strides[i] = st[i];
  const auto &decl = set.decls[row.decl];
  r.declCount = static_cast<u32>(decl.size() / sizeof(DeclElement));
  std::memcpy(r.declRaw, decl.data(), decl.size());
  s.depthBias = 0;
  s.slopeScaledDepthBias = 0.0f;
  s.targetScale = 0.0f;
  switch (row.bias) {
  case PsoBias::None:
    break;
  case PsoBias::Material:
    s.depthBias = PolygonOffsetUnits(row.m * 1.0e-5f);
    s.slopeScaledDepthBias = row.m * 2.0f;
    break;
  case PsoBias::Shadow:
    if (!shadow)
      return false;
    s.depthBias = shadow->depthBias;
    s.slopeScaledDepthBias = shadow->slope;
    break;
  case PsoBias::Literal:
    s.depthBias = set.literals[row.literal].depthBias;
    s.slopeScaledDepthBias = set.literals[row.literal].slope;
    break;
  }
  PsoApplyTargetScale(s);
  const ShaderCacheEntry *vs = FindShaderCacheEntry(s.vsHash);
  if (!vs)
    return false;
  std::vector<VertexInput> inputs;
  VertexInputsFromEntry(*vs, inputs);
  const InputLayout *layout = GetInputLayoutFromRaw(s.vsHash, inputs, r.declRaw, r.declCount);
  if (!layout)
    return false;
  s.layoutKey = layout->key;
  s.spec = (s.spec & ~kSpecLayoutBits) | layout->spec;
  u32 spec_mask = vs->specConstantsMask;
  if (s.psHash)
    if (const ShaderCacheEntry *e = FindShaderCacheEntry(s.psHash))
      spec_mask |= e->specConstantsMask;
  CanonicalizePipelineState(s, spec_mask, layout->streamMask);
  *out = r;
  return true;
}

}
