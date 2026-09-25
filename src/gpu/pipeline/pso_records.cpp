#include "gpu/pipeline/pso_records.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <atomic>
#include <string_view>
#endif

#include "core/logging.h"
#include "gpu/format.h"
#include "gpu/settings.h"
#include "gpu/shaders/guest_shaders.h"
#include "embedded_pipeline_list.h"

namespace eot::gpu {

namespace {

enum Col : u32 {
  cVsHash, cPsHash, cSpec, cLayoutKey, cDeclRaw, cStrides, cTopology, cRtFormats, cRtCount,
  cDsFormat, cSampleCount, cCull, cFrontFace, cDepthBias, cSlopeScaledDepthBias, cTargetScale,
  cDepthClip, cDepthEnable, cDepthWrite, cDepthFunc, cStencilEnable, cStencilReadMask,
  cStencilWriteMask, cStencilRef, cStencilFront, cStencilBack, cBlend0, cBlend1, cBlend2, cBlend3,
  cAlphaToCoverage, cVelocity, cMsaa, cFrame, cSession, cPackage, cCount
};
static_assert(cCount == PsoCsvLayout::kColumns);

constexpr const char *kColumnNames[cCount] = {
    "vsHash", "psHash", "spec", "layoutKey", "declRaw", "strides", "topology", "rtFormats",
    "rtCount", "dsFormat", "sampleCount", "cull", "frontFace", "depthBias",
    "slopeScaledDepthBias", "targetScale", "depthClip", "depthEnable", "depthWrite", "depthFunc",
    "stencilEnable", "stencilReadMask", "stencilWriteMask", "stencilRef", "stencilFront",
    "stencilBack", "blend0", "blend1", "blend2", "blend3", "alphaToCoverage", "velocity", "msaa",
    "frame", "session", "package"};

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

f32 FollowTargetScale(const PipelineState &s) {
  const bool caster = s.rtCount == 0 && s.dsFormat != plume::RenderFormat::UNKNOWN;
  return caster ? ShadowMapTargetScale() : RenderScaleFactor();
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

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
    s.remove_suffix(1);
  while (!s.empty() && s.front() == ' ')
    s.remove_prefix(1);
  return s;
}

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

std::string SessionTagImpl() {
  char raw[256] = {};
#if defined(_WIN32)
  DWORD n = sizeof(raw);
  if (!::GetComputerNameA(raw, &n))
    raw[0] = 0;
#else
  if (gethostname(raw, sizeof(raw) - 1) != 0)
    raw[0] = 0;
#endif
  std::string tag;
  for (const char *p = raw; *p && tag.size() < 24; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (std::isalnum(c))
      tag.push_back(static_cast<char>(std::tolower(c)));
  }
  return tag.empty() ? "session" : tag;
}

}

void PsoApplyTargetScale(PipelineState &s) {
  const bool follows = s.targetScale == 0.0f;
  if (s.depthBias || s.slopeScaledDepthBias != 0.0f) {
    s.targetScale = follows ? FollowTargetScale(s) : s.targetScale;
    if (follows && s.rtCount == 0 && s.sampleCount <= 1 && !s.stencilEnable && IsDepthFormat(s.dsFormat))
      s.dsFormat = DepthRenderTargetFormat();
  } else {
    s.targetScale = 1.0f;
  }
}

std::string PsoCsvHeader() {
  std::string h = std::format("# eot-pso v{}\n", kPsoCsvVersion);
  for (u32 i = 0; i < cCount; ++i)
    h += std::format("{}{}", i ? "," : "", kColumnNames[i]);
  return h;
}

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
  std::string packages;
  for (u32 i = 0; i < r.packageCount && i < kMaxRecordPackages; ++i)
    packages += std::format("{}{:x}", i ? "|" : "", r.packages[i]);
  return std::format(
      "{:016x},{:016x},{:x},{:016x},{},{},{},{},{},{},{},{},{},{},{:.9g},{:.9g},{},{},{},{},{},{},"
      "{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
      s.vsHash, s.psHash, s.spec, s.layoutKey, decl, strides, ei(s.topology), rts, s.rtCount,
      ei(s.dsFormat), s.sampleCount, ei(s.cull), ei(s.frontFace), s.depthBias,
      s.slopeScaledDepthBias,
      std::fabs(s.targetScale - FollowTargetScale(s)) < 0.01f ? 0.0f : s.targetScale,
      s.depthClip ? 1 : 0, s.depthEnable ? 1 : 0, s.depthWrite ? 1 : 0, ei(s.depthFunc),
      s.stencilEnable ? 1 : 0, static_cast<unsigned>(s.stencilReadMask),
      static_cast<unsigned>(s.stencilWriteMask), static_cast<unsigned>(s.stencilRef),
      Stencil(s.stencilFront), Stencil(s.stencilBack), Blend(s.blend[0]), Blend(s.blend[1]),
      Blend(s.blend[2]), Blend(s.blend[3]), s.alphaToCoverage ? 1 : 0, s.velocity ? 1 : 0, r.msaa,
      r.frame, session, packages);
}

bool PsoCsvParseHeader(std::string_view line, PsoCsvLayout *out) {
  line = Trim(line);
  if (!line.starts_with("vsHash"))
    return false;
  for (u32 i = 0; i < PsoCsvLayout::kColumns; ++i)
    out->index[i] = -1;
  const auto names = Split(line, ',');
  out->fieldCount = static_cast<u32>(names.size());
  for (u32 pos = 0; pos < names.size() && pos < 127; ++pos) {
    for (u32 c = 0; c < cCount; ++c) {
      if (names[pos] == kColumnNames[c]) {
        out->index[c] = static_cast<i8>(pos);
        break;
      }
    }
  }
  out->version = out->index[cTargetScale] < 0    ? 1
                 : out->index[cPackage] < 0      ? 2
                 : out->index[cVelocity] < 0     ? 3
                 : out->index[cMsaa] < 0         ? 4
                                                 : 5;
  return true;
}

bool PsoRecordFromCsv(const PsoCsvLayout &layout, std::string_view line, PsoRecord *out) {
  line = Trim(line);
  if (line.empty() || line[0] == '#' || line.starts_with("vsHash"))
    return false;
  const auto c = Split(line, ',');
  if (c.size() != layout.fieldCount)
    return false;
  auto field = [&](u32 col) -> std::string_view {
    const int i = layout.index[col];
    return i < 0 ? std::string_view() : c[static_cast<size_t>(i)];
  };
  PsoRecord r{};
  ZeroPipelineState(r.state);
  PipelineState &s = r.state;
  u64 u;
  i64 i;
  if (!ParseU64(field(cVsHash), &s.vsHash, 16) || !ParseU64(field(cPsHash), &s.psHash, 16) ||
      !ParseU64(field(cSpec), &u, 16))
    return false;
  s.spec = static_cast<u32>(u);
  if (!ParseU64(field(cLayoutKey), &s.layoutKey, 16))
    return false;
  const std::string_view hex = field(cDeclRaw);
  if (hex.size() % (2 * sizeof(DeclElement)) != 0 || hex.size() / 2 > sizeof(r.declRaw))
    return false;
  for (size_t k = 0; k < hex.size(); k += 2) {
    const int hi = HexVal(hex[k]), lo = HexVal(hex[k + 1]);
    if (hi < 0 || lo < 0)
      return false;
    r.declRaw[k / 2] = static_cast<u8>(hi << 4 | lo);
  }
  r.declCount = static_cast<u32>(hex.size() / (2 * sizeof(DeclElement)));
  const auto strides = Split(field(cStrides), '|');
  if (strides.size() != 16)
    return false;
  for (u32 k = 0; k < 16; ++k) {
    if (!ParseI64(strides[k], &i))
      return false;
    s.strides[k] = static_cast<u32>(i);
  }
  if (!ParseEnum(field(cTopology), &s.topology))
    return false;
  const auto rts = Split(field(cRtFormats), '|');
  if (rts.size() != 4)
    return false;
  for (u32 k = 0; k < 4; ++k)
    if (!ParseEnum(rts[k], &s.rtFormats[k]))
      return false;
  if (!ParseI64(field(cRtCount), &i))
    return false;
  s.rtCount = static_cast<u32>(i);
  if (!ParseEnum(field(cDsFormat), &s.dsFormat) || !ParseI64(field(cSampleCount), &i))
    return false;
  s.sampleCount = static_cast<u32>(i);
  if (!ParseEnum(field(cCull), &s.cull) || !ParseEnum(field(cFrontFace), &s.frontFace) ||
      !ParseI64(field(cDepthBias), &i))
    return false;
  s.depthBias = static_cast<i32>(i);
  if (!ParseF32(field(cSlopeScaledDepthBias), &s.slopeScaledDepthBias))
    return false;
  if (layout.version >= 2) {
    if (!ParseF32(field(cTargetScale), &s.targetScale))
      return false;
  } else if (s.depthBias) {
    const i32 b = s.depthBias;
    const i32 layers = static_cast<i32>(std::ceil((std::abs(b) + 0.5) / 4.0));
    s.depthBias = (b < 0 ? -layers : layers) * 8;
    s.targetScale = 0.0f;
  }
  if (!ParseBool(field(cDepthClip), &s.depthClip) || !ParseBool(field(cDepthEnable), &s.depthEnable) ||
      !ParseBool(field(cDepthWrite), &s.depthWrite) || !ParseEnum(field(cDepthFunc), &s.depthFunc) ||
      !ParseBool(field(cStencilEnable), &s.stencilEnable))
    return false;
  i64 m0, m1, m2;
  if (!ParseI64(field(cStencilReadMask), &m0) || !ParseI64(field(cStencilWriteMask), &m1) ||
      !ParseI64(field(cStencilRef), &m2))
    return false;
  s.stencilReadMask = static_cast<u8>(m0);
  s.stencilWriteMask = static_cast<u8>(m1);
  s.stencilRef = static_cast<u8>(m2);
  if (!ParseStencil(field(cStencilFront), &s.stencilFront) ||
      !ParseStencil(field(cStencilBack), &s.stencilBack))
    return false;
  for (u32 k = 0; k < 4; ++k)
    if (!ParseBlend(field(cBlend0 + k), &s.blend[k]))
      return false;
  if (!ParseBool(field(cAlphaToCoverage), &s.alphaToCoverage))
    return false;
  PsoApplyTargetScale(s);
  s.velocity = false;
  if (layout.index[cVelocity] >= 0 && !ParseBool(field(cVelocity), &s.velocity))
    return false;
  if (layout.index[cMsaa] >= 0 && ParseU64(field(cMsaa), &u) && u <= 3)
    r.msaa = static_cast<u8>(u);
  ParseU64(field(cFrame), &r.frame);
  const std::string_view pk = field(cPackage);
  if (!pk.empty()) {
    for (const auto p : Split(pk, '|')) {
      if (ParseU64(p, &u, 16) && u < 0x1000 && r.packageCount < kMaxRecordPackages)
        r.packages[r.packageCount++] = static_cast<u16>(u);
    }
  }
  u32 spec_mask = ~0u;
  if (s.vsHash) {
    spec_mask = 0;
    if (const ShaderCacheEntry *e = FindShaderCacheEntry(s.vsHash))
      spec_mask |= e->specConstantsMask;
    if (s.psHash) {
      if (const ShaderCacheEntry *e = FindShaderCacheEntry(s.psHash))
        spec_mask |= e->specConstantsMask;
    }
  }
  CanonicalizePipelineState(s, spec_mask, 0xFFFFu);
  *out = r;
  return true;
}

std::string PsoSessionStamp() {
  static const std::string stamp = SessionStampImpl();
  return stamp;
}

std::string PsoSessionTag() {
  static const std::string tag = SessionTagImpl();
  return tag;
}

size_t ParsePsoCsv(std::string_view data, std::vector<PsoRecord> &out, size_t *bad) {
  size_t rows = 0;
  PsoCsvLayout layout{};
  bool have_layout = false;
  for (const auto line : Split(data, '\n')) {
    if (!have_layout) {
      have_layout = PsoCsvParseHeader(line, &layout);
      continue;
    }
    PsoRecord r;
    if (PsoRecordFromCsv(layout, line, &r)) {
      out.push_back(r);
      ++rows;
    } else if (bad && !Trim(line).empty() && line[0] != '#') {
      ++*bad;
    }
  }
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
    if (!name.starts_with("pso_drawn_"))
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
    size_t bad = 0;
    const size_t rows = ParsePsoCsv(data, out, &bad);
    n += rows;
    EOT_DEBUG("[pso] {}: {} rows{}", name, rows, bad ? std::format(" ({} unparsable)", bad) : "");
  }
  return n;
}

void PsoCaptureConfigure() {
  auto &c = capture();
  std::lock_guard lock(c.mutex);
  std::error_code ec;
  std::filesystem::create_directories(kPsoDir, ec);
  c.dir = kPsoDir;
  c.tag = PsoSessionTag();
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
    c.path = (std::filesystem::path(c.dir) /
              ("pso_drawn_" + c.tag + "_" + PsoSessionStamp() + ".csv"))
                 .string();
  }
  c.pending.push_back(PsoRecordToCsv(r, c.tag));
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
  EOT_DEBUG("[pso] {} pipeline(s) drawn -> {} ({} this session)", c.pending.size(), c.path,
            c.written);
  c.pending.clear();
}

}

namespace eot::gpu {

namespace {

PsoList g_list;
std::atomic<bool> g_ready{false};
std::once_flag g_once;

struct Builder {
  PsoList &list;
  std::unordered_map<u64, u32> index;
  std::vector<bool> boot;

  void Add(const PsoRecord &r, PsoSource source) {
    const u64 key = HashPipelineState(r.state);
    const auto [it, fresh] = index.try_emplace(key, static_cast<u32>(list.rows.size()));
    const u32 row = it->second;
    if (fresh) {
      list.rows.push_back(r);
      list.rows.back().packageCount = 0;
      list.sources.push_back(source);
      boot.push_back(false);
      list.byPair[PsoPairKey(r.state.vsHash, r.state.psHash)].push_back(row);
    } else {
      PsoRecord &e = list.rows[row];
      e.msaa = e.msaa == 0 ? r.msaa : r.msaa == 0 ? e.msaa : static_cast<u8>(e.msaa | r.msaa);
      list.merged++;
    }
    const bool at_boot =
        r.packageCount == 0 || std::find(r.packages, r.packages + r.packageCount, 0) != r.packages + r.packageCount;
    if (at_boot && !boot[row]) {
      boot[row] = true;
      list.boot.push_back(row);
    }
    PsoRecord &e = list.rows[row];
    for (u32 i = 0; i < r.packageCount; ++i) {
      const u16 p = r.packages[i];
      if (p == 0 || std::find(e.packages, e.packages + e.packageCount, p) != e.packages + e.packageCount)
        continue;
      if (e.packageCount < kMaxRecordPackages)
        e.packages[e.packageCount++] = p;
      list.byPackage[p].push_back(row);
    }
  }
};

}

u64 PsoPairKey(u64 vs_hash, u64 ps_hash) { return vs_hash ^ (ps_hash * 0x9E3779B97F4A7C15ull); }

void PsoListLoad() {
  std::call_once(g_once, [] {
    const auto start = std::chrono::steady_clock::now();
    Builder b{g_list, {}, {}};
    std::vector<PsoRecord> rows;
    size_t bad = 0;
    const EmbeddedAsset shipped = EmbeddedPipelineList();
    g_list.shipped = static_cast<u32>(ParsePsoCsv(shipped.text(), rows, &bad));
    for (const PsoRecord &r : rows)
      b.Add(r, PsoSource::Recorded);
    rows.clear();
    g_list.local = static_cast<u32>(LoadPsoCsvDir(kPsoDir, rows));
    for (const PsoRecord &r : rows)
      b.Add(r, PsoSource::Local);
    g_list.unparsed = static_cast<u32>(bad);
    g_ready.store(true, std::memory_order_release);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EOT_INFO("[pso] pipeline list: {} rows ({} shipped, {} from this machine's captures, {} merged{}) "
             "over {} shader pairs and {} packages, {} at boot, read in {:.0f} ms",
             g_list.rows.size(), g_list.shipped, g_list.local, g_list.merged,
             bad ? std::format(", {} unparsable", bad) : "", g_list.byPair.size(),
             g_list.byPackage.size(), g_list.boot.size(), ms);
  });
}

const PsoList &PsoListGet() {
  static const PsoList empty;
  return g_ready.load(std::memory_order_acquire) ? g_list : empty;
}

}
