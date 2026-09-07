#include "gpu/pipeline/pso_records.h"

#include <charconv>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "core/logging.h"
#include "gpu/settings.h"

namespace eot::gpu {

namespace {

constexpr const char *kColumns =
    "vsHash,psHash,spec,layoutKey,declRaw,strides,topology,rtFormats,rtCount,dsFormat,"
    "sampleCount,cull,frontFace,depthBias,slopeScaledDepthBias,targetScale,depthClip,depthEnable,"
    "depthWrite,depthFunc,stencilEnable,stencilReadMask,stencilWriteMask,stencilRef,"
    "stencilFront,stencilBack,blend0,blend1,blend2,blend3,alphaToCoverage,frame,session";
constexpr u32 kColumnCount = 33;

const char kHexDigits[] = "0123456789abcdef";

template <typename E> int ei(E e) { return static_cast<int>(e); }

std::string Stencil(const plume::RenderStencilFaceDesc &f) {
  return std::format("{}|{}|{}|{}", ei(f.passOp), ei(f.failOp), ei(f.depthFailOp),
                     ei(f.compareFunction));
}

std::string Blend(const plume::RenderBlendDesc &b) {
  return std::format("{}|{}|{}|{}|{}|{}|{}|{}", b.blendEnabled ? 1 : 0, ei(b.srcBlend),
                     ei(b.dstBlend), ei(b.blendOp), ei(b.srcBlendAlpha), ei(b.dstBlendAlpha),
                     ei(b.blendOpAlpha), static_cast<unsigned>(b.renderTargetWriteMask));
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
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

bool ParseU64(std::string_view s, u64 *v, int base = 10) {
  if (s.empty())
    return false;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), *v, base);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool ParseI64(std::string_view s, i64 *v) {
  if (s.empty())
    return false;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), *v, 10);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool ParseF32(std::string_view s, f32 *v) {
  if (s.empty())
    return false;
  const std::string tmp(s);
  char *end = nullptr;
  *v = std::strtof(tmp.c_str(), &end);
  return end && *end == '\0';
}

template <typename E> bool ParseEnum(std::string_view s, E *e) {
  i64 v;
  if (!ParseI64(s, &v))
    return false;
  *e = static_cast<E>(v);
  return true;
}

bool ParseBool(std::string_view s, bool *b) {
  i64 v;
  if (!ParseI64(s, &v))
    return false;
  *b = v != 0;
  return true;
}

bool ParseStencil(std::string_view s, plume::RenderStencilFaceDesc *f) {
  const auto p = Split(s, '|');
  return p.size() == 4 && ParseEnum(p[0], &f->passOp) && ParseEnum(p[1], &f->failOp) &&
         ParseEnum(p[2], &f->depthFailOp) && ParseEnum(p[3], &f->compareFunction);
}

bool ParseBlend(std::string_view s, plume::RenderBlendDesc *b) {
  const auto p = Split(s, '|');
  i64 mask;
  if (p.size() != 8 || !ParseBool(p[0], &b->blendEnabled) || !ParseEnum(p[1], &b->srcBlend) ||
      !ParseEnum(p[2], &b->dstBlend) || !ParseEnum(p[3], &b->blendOp) ||
      !ParseEnum(p[4], &b->srcBlendAlpha) || !ParseEnum(p[5], &b->dstBlendAlpha) ||
      !ParseEnum(p[6], &b->blendOpAlpha) || !ParseI64(p[7], &mask))
    return false;
  b->renderTargetWriteMask = static_cast<u8>(mask);
  return true;
}

int HexVal(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

const char *const kCompiledInRows[] = {
#include "gpu/pipeline/cache/eot_pipelines.inc"
    nullptr};
const char *const kCompiledInTemplateRows[] = {
#include "gpu/pipeline/cache/eot_pso_templates.inc"
    nullptr};

struct Capture {
  std::mutex mutex;
  std::string dir, tag, path;
  std::vector<std::string> pending;
  bool headerWritten = false;
  u64 lastFlushFrame = 0;
  u32 written = 0;
};

Capture &capture() {
  static Capture c;
  return c;
}

std::string SessionId(const std::string &tag) {
  return tag.empty() ? PsoSessionStamp() : tag + "_" + PsoSessionStamp();
}

std::string SessionStampImpl() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

}

std::string PsoCsvHeader() { return std::format("# eot-pso v{}\n{}", kPsoCsvVersion, kColumns); }

std::string PsoRecordToCsv(const PsoRecord &r, std::string_view session) {
  const PipelineState &s = r.state;
  std::string decl;
  decl.reserve(r.declCount * sizeof(DeclElement) * 2);
  for (u32 i = 0; i < r.declCount * sizeof(DeclElement) && i < sizeof(r.declRaw); ++i) {
    decl.push_back(kHexDigits[r.declRaw[i] >> 4]);
    decl.push_back(kHexDigits[r.declRaw[i] & 15]);
  }
  std::string strides;
  for (u32 i = 0; i < 16; ++i)
    strides += std::format("{}{}", i ? "|" : "", s.strides[i]);
  std::string rts;
  for (u32 i = 0; i < 4; ++i)
    rts += std::format("{}{}", i ? "|" : "", ei(s.rtFormats[i]));
  return std::format(
      "{:016x},{:016x},{:x},{:016x},{},{},{},{},{},{},{},{},{},{},{:.9g},{:.9g},{},{},{},{},{},{},"
      "{},{},{},{},{},{},{},{},{},{},{}",
      s.vsHash, s.psHash, s.spec, s.layoutKey, decl, strides, ei(s.topology), rts, s.rtCount,
      ei(s.dsFormat), s.sampleCount, ei(s.cull), ei(s.frontFace), s.depthBias,
      s.slopeScaledDepthBias,
      std::fabs(s.targetScale - RenderScaleFactor()) < 0.01f ? 0.0f : s.targetScale,
      s.depthClip ? 1 : 0, s.depthEnable ? 1 : 0, s.depthWrite ? 1 : 0,
      ei(s.depthFunc), s.stencilEnable ? 1 : 0, static_cast<unsigned>(s.stencilReadMask),
      static_cast<unsigned>(s.stencilWriteMask), static_cast<unsigned>(s.stencilRef),
      Stencil(s.stencilFront), Stencil(s.stencilBack), Blend(s.blend[0]), Blend(s.blend[1]),
      Blend(s.blend[2]), Blend(s.blend[3]), s.alphaToCoverage ? 1 : 0, r.frame, session);
}

bool PsoRecordFromCsv(std::string_view line, PsoRecord *out) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
    line.remove_suffix(1);
  if (line.empty() || line[0] == '#' || line.starts_with("vsHash"))
    return false;
  const auto c = Split(line, ',');
  if (c.size() != kColumnCount)
    return false;
  PsoRecord r{};
  ZeroPipelineState(r.state);
  PipelineState &s = r.state;
  u64 u;
  i64 i;
  if (!ParseU64(c[0], &s.vsHash, 16) || !ParseU64(c[1], &s.psHash, 16) ||
      !ParseU64(c[2], &u, 16))
    return false;
  s.spec = static_cast<u32>(u);
  if (!ParseU64(c[3], &s.layoutKey, 16))
    return false;
  const std::string_view hex = c[4];
  if (hex.size() % (2 * sizeof(DeclElement)) != 0 || hex.size() / 2 > sizeof(r.declRaw))
    return false;
  for (size_t k = 0; k < hex.size(); k += 2) {
    const int hi = HexVal(hex[k]), lo = HexVal(hex[k + 1]);
    if (hi < 0 || lo < 0)
      return false;
    r.declRaw[k / 2] = static_cast<u8>(hi << 4 | lo);
  }
  r.declCount = static_cast<u32>(hex.size() / (2 * sizeof(DeclElement)));
  const auto strides = Split(c[5], '|');
  if (strides.size() != 16)
    return false;
  for (u32 k = 0; k < 16; ++k) {
    if (!ParseI64(strides[k], &i))
      return false;
    s.strides[k] = static_cast<u32>(i);
  }
  if (!ParseEnum(c[6], &s.topology))
    return false;
  const auto rts = Split(c[7], '|');
  if (rts.size() != 4)
    return false;
  for (u32 k = 0; k < 4; ++k)
    if (!ParseEnum(rts[k], &s.rtFormats[k]))
      return false;
  if (!ParseI64(c[8], &i))
    return false;
  s.rtCount = static_cast<u32>(i);
  if (!ParseEnum(c[9], &s.dsFormat) || !ParseI64(c[10], &i))
    return false;
  s.sampleCount = static_cast<u32>(i);
  if (!ParseEnum(c[11], &s.cull) || !ParseEnum(c[12], &s.frontFace) || !ParseI64(c[13], &i))
    return false;
  s.depthBias = static_cast<i32>(i);
  if (!ParseF32(c[14], &s.slopeScaledDepthBias) || !ParseF32(c[15], &s.targetScale) ||
      !ParseBool(c[16], &s.depthClip) || !ParseBool(c[17], &s.depthEnable) ||
      !ParseBool(c[18], &s.depthWrite) || !ParseEnum(c[19], &s.depthFunc) ||
      !ParseBool(c[20], &s.stencilEnable))
    return false;
  if (s.depthBias || s.slopeScaledDepthBias != 0.0f)
    s.targetScale = s.targetScale == 0.0f ? RenderScaleFactor() : s.targetScale;
  else
    s.targetScale = 1.0f;
  i64 m0, m1, m2;
  if (!ParseI64(c[21], &m0) || !ParseI64(c[22], &m1) || !ParseI64(c[23], &m2))
    return false;
  s.stencilReadMask = static_cast<u8>(m0);
  s.stencilWriteMask = static_cast<u8>(m1);
  s.stencilRef = static_cast<u8>(m2);
  if (!ParseStencil(c[24], &s.stencilFront) || !ParseStencil(c[25], &s.stencilBack))
    return false;
  for (u32 k = 0; k < 4; ++k)
    if (!ParseBlend(c[26 + k], &s.blend[k]))
      return false;
  if (!ParseBool(c[30], &s.alphaToCoverage))
    return false;
  ParseU64(c[31], &r.frame);
  *out = r;
  return true;
}

const std::vector<PsoRecord> &CompiledInPipelines() {
  static const std::vector<PsoRecord> rows = [] {
    std::vector<PsoRecord> v;
    u32 bad = 0;
    for (const char *const *p = kCompiledInRows; *p; ++p) {
      PsoRecord r;
      if (PsoRecordFromCsv(*p, &r))
        v.push_back(r);
      else if (**p != '#' && std::string_view(*p).substr(0, 6) != "vsHash")
        ++bad;
    }
    if (bad)
      EOT_WARN("[pso] {} compiled-in rows did not parse (schema v{}); regenerate "
               "cache/eot_pipelines.inc with tools/pso/pso_merge.py",
               bad, kPsoCsvVersion);
    return v;
  }();
  return rows;
}

std::string PsoSessionStamp() {
  static const std::string stamp = SessionStampImpl();
  return stamp;
}

const std::vector<PsoTemplate> &CompiledInTemplates() {
  static const std::vector<PsoTemplate> rows = [] {
    std::vector<PsoTemplate> v;
    u32 bad = 0;
    for (const char *const *p = kCompiledInTemplateRows; *p; ++p) {
      std::string_view line(*p);
      if (line.empty() || line[0] == '#' || line.starts_with("technique"))
        continue;
      const size_t c1 = line.find(',');
      const size_t c2 = c1 == std::string_view::npos ? c1 : line.find(',', c1 + 1);
      const size_t c3 = c2 == std::string_view::npos ? c2 : line.find(',', c2 + 1);
      u64 tech = 0, pass = 0, cls = 0;
      PsoRecord r;
      if (c3 == std::string_view::npos || !ParseU64(line.substr(0, c1), &tech) ||
          !ParseU64(line.substr(c1 + 1, c2 - c1 - 1), &pass) ||
          !ParseU64(line.substr(c2 + 1, c3 - c2 - 1), &cls, 16) ||
          !PsoRecordFromCsv(line.substr(c3 + 1), &r) || tech > 255 || pass > 255) {
        ++bad;
        continue;
      }
      v.push_back(PsoTemplate{static_cast<u8>(tech), static_cast<u8>(pass), static_cast<u32>(cls), r.state});
    }
    if (bad)
      EOT_WARN("[pso] {} compiled-in template rows did not parse; regenerate "
               "cache/eot_pso_templates.inc with tools/pso/pso_gen_templates.py", bad);
    return v;
  }();
  return rows;
}

size_t LoadPsoCsvDir(const std::string &dir, std::vector<PsoRecord> &out) {
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec))
    return 0;
  size_t n = 0;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".csv")
      continue;
    const std::string name = entry.path().filename().string();
    if (name.starts_with("pso_pairs_") || name.starts_with("pso_predicted_"))
      continue;
    FILE *f = std::fopen(entry.path().string().c_str(), "rb");
    if (!f)
      continue;
    std::string data;
    char buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
      data.append(buf, got);
    std::fclose(f);
    size_t rows = 0, bad = 0;
    bool version_ok = true;
    for (const auto line : Split(data, '\n')) {
      if (line.starts_with("# eot-pso v")) {
        u64 v = 0;
        version_ok = ParseU64(line.substr(11), &v) && v == kPsoCsvVersion;
        if (!version_ok) {
          EOT_WARN("[pso] {}: schema v{} != v{}; ignored", entry.path().filename().string(), v,
                   kPsoCsvVersion);
          break;
        }
        continue;
      }
      PsoRecord r;
      if (PsoRecordFromCsv(line, &r)) {
        out.push_back(r);
        ++rows;
      } else if (!line.empty() && line[0] != '#' && !line.starts_with("vsHash") &&
                 line != "\r") {
        ++bad;
      }
    }
    if (version_ok) {
      n += rows;
      EOT_INFO("[pso] {}: {} rows{}", entry.path().filename().string(), rows,
               bad ? std::format(" ({} unparsable)", bad) : "");
    }
  }
  return n;
}

void PsoCaptureConfigure(const std::string &dir, const std::string &tag) {
  auto &c = capture();
  std::lock_guard lock(c.mutex);
  c.dir = dir;
  c.tag = tag;
  c.path.clear();
  c.headerWritten = false;
}

void PsoCaptureAdd(const PsoRecord &r) {
  auto &c = capture();
  std::lock_guard lock(c.mutex);
  if (c.dir.empty())
    return;
  if (c.path.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(c.dir, ec);
    c.path = (std::filesystem::path(c.dir) / ("pso_misses_" + SessionId(c.tag) + ".csv")).string();
  }
  c.pending.push_back(PsoRecordToCsv(r, c.tag.empty() ? "-" : c.tag));
}

void PsoCaptureFlush(bool force, u64 guest_frame) {
  auto &c = capture();
  std::lock_guard lock(c.mutex);
  if (c.pending.empty() || c.path.empty())
    return;
  if (!force && guest_frame < c.lastFlushFrame + 300)
    return;
  c.lastFlushFrame = guest_frame;
  FILE *f = std::fopen(c.path.c_str(), c.headerWritten ? "ab" : "wb");
  if (!f) {
    EOT_WARN("[pso] cannot write {}; capture disabled", c.path);
    c.dir.clear();
    return;
  }
  if (!c.headerWritten) {
    const std::string h = PsoCsvHeader() + "\n";
    std::fwrite(h.data(), 1, h.size(), f);
    c.headerWritten = true;
  }
  for (const std::string &row : c.pending) {
    std::fwrite(row.data(), 1, row.size(), f);
    std::fputc('\n', f);
  }
  std::fclose(f);
  c.written += static_cast<u32>(c.pending.size());
  EOT_INFO("[pso] {} new pipeline(s) captured -> {} ({} this session)", c.pending.size(), c.path,
           c.written);
  c.pending.clear();
}

}
