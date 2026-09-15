#include "goliath/text/translation.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/ui/menu_handles.h"

REX_EXTERN(__imp__eot_PKPackage_FindString);
REX_EXTERN(__imp__eot_MMMemoryMgr_Alloc);

namespace eot::text {
namespace {

constexpr uint32_t kPackageId = 176;
constexpr uint32_t kPackageTables = 476;
constexpr uint32_t kPackageTableCount = 480;
constexpr uint32_t kTableRecordSize = 24;
constexpr uint32_t kTableEntries = 0;
constexpr uint32_t kTableText = 8;
constexpr uint32_t kTableEntryCount = 12;
constexpr uint32_t kEntrySize = 12;
constexpr uint32_t kEntryLineCount = 4;
constexpr uint32_t kEntryLines = 8;
constexpr uint32_t kLineSize = 12;

struct Shipped {
  const char *language;
  uint32_t package;
  uint32_t salt;
  bool fold;
};
constexpr Shipped kShipped[] = {
    {"ru", eot::ui::kReeotRussianPackageId, 0x52555353u, true},
};

struct Line {
  std::u16string text;
  uint32_t guest = 0;
};

std::unordered_map<uint64_t, uint32_t> g_index;
std::vector<Line> g_lines;
uint32_t g_block = 0;
size_t g_block_bytes = 0;
bool g_block_failed = false;
std::string g_language;
bool g_key_by_package = true;
const Shipped *g_shipped = nullptr;
uint32_t g_shipped_object = 0;

uint64_t Key(uint32_t package, uint32_t crc, uint32_t line) {
  return (static_cast<uint64_t>(package & 0xFFF) << 40) | (static_cast<uint64_t>(crc) << 8) | (line & 0xFF);
}

char16_t FoldCyrillic(char16_t c) {
  static constexpr char16_t kUpper[] = u"\u0410\u0412\u0415\u041A\u041C\u041D\u041E\u0420\u0421\u0422\u0425";
  static constexpr char16_t kUpperLatin[] = u"ABEKMHOPCTX";
  static constexpr char16_t kLower[] = u"\u0430\u0435\u043E\u0440\u0441\u0443\u0445";
  static constexpr char16_t kLowerLatin[] = u"aeopcyx";
  for (size_t i = 0; kUpper[i]; ++i)
    if (c == kUpper[i])
      return kUpperLatin[i];
  for (size_t i = 0; kLower[i]; ++i)
    if (c == kLower[i])
      return kLowerLatin[i];
  if (c >= 0x0410 && c <= 0x044F)
    return static_cast<char16_t>(0xC0 + (c - 0x0410));
  if (c == 0x0401)
    return 0xA8;
  if (c == 0x0451)
    return 0xB8;
  return c;
}

std::u16string Decode(std::string_view escaped) {
  std::string utf8;
  utf8.reserve(escaped.size());
  for (size_t i = 0; i < escaped.size(); ++i) {
    const char c = escaped[i];
    if (c == '\\' && i + 1 < escaped.size()) {
      const char n = escaped[++i];
      utf8.push_back(n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n);
    } else {
      utf8.push_back(c);
    }
  }
  std::u16string out;
  out.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size();) {
    const auto lead = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = lead;
    size_t length = 1;
    if (lead >= 0xF0) {
      cp = lead & 0x07;
      length = 4;
    } else if (lead >= 0xE0) {
      cp = lead & 0x0F;
      length = 3;
    } else if (lead >= 0xC0) {
      cp = lead & 0x1F;
      length = 2;
    }
    if (i + length > utf8.size())
      break;
    for (size_t k = 1; k < length; ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
    i += length;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

void CopyIntoGuest(PPCContext &ctx, uint8_t *base) {
  if (g_block || g_block_failed)
    return;
  size_t bytes = 0;
  for (const Line &line : g_lines)
    bytes += (line.text.size() + 1) * 2;
  PPCContext call = ctx;
  call.r3.u32 = static_cast<uint32_t>(bytes);
  call.r4.u32 = 16;
  call.r5.u32 = 0xFFFFFFFFu;
  call.r6.u32 = 0;
  __imp__eot_MMMemoryMgr_Alloc(call, base);
  g_block = call.r3.u32;
  if (!g_block) {
    g_block_failed = true;
    EOT_WARN("[text] no guest memory for the {} translation ({} bytes); the game's own text stands", g_language,
             bytes);
    return;
  }
  uint32_t at = g_block;
  for (Line &line : g_lines) {
    line.guest = at;
    for (const char16_t c : line.text) {
      eot::mem::store<uint16_t>(at, static_cast<uint16_t>(c));
      at += 2;
    }
    eot::mem::store<uint16_t>(at, 0);
    at += 2;
  }
  g_block_bytes = bytes;
  EOT_INFO("[text] {} translation: {} lines in the guest at {:#x} ({} bytes)", g_language, g_lines.size(), g_block,
           bytes);
}

bool IndexShipped() {
  const uint32_t package = g_shipped_object;
  if (!package)
    return false;
  const uint32_t tables = eot::mem::load<uint32_t>(package + kPackageTables);
  const uint32_t count = eot::mem::load<uint32_t>(package + kPackageTableCount);
  if (!tables || !count)
    return false;
  size_t lines = 0;
  for (uint32_t t = 0; t < count; ++t) {
    const uint32_t record = tables + t * kTableRecordSize;
    const uint32_t entries = eot::mem::load<uint32_t>(record + kTableEntries);
    const uint32_t text = eot::mem::load<uint32_t>(record + kTableText);
    const uint32_t n = eot::mem::load<uint32_t>(record + kTableEntryCount);
    if (!entries || !text)
      continue;
    for (uint32_t s = 0; s < n; ++s) {
      const uint32_t entry = entries + s * kEntrySize;
      const uint32_t crc = eot::mem::load<uint32_t>(entry) ^ g_shipped->salt;
      const uint32_t lineCount = eot::mem::load<uint32_t>(entry + kEntryLineCount);
      const uint32_t lineRecords = eot::mem::load<uint32_t>(entry + kEntryLines);
      for (uint32_t l = 0; l < lineCount && l < 256; ++l) {
        const uint32_t offset = eot::mem::load<uint32_t>(lineRecords + l * kLineSize);
        g_index[Key(0, crc, l)] = static_cast<uint32_t>(g_lines.size());
        g_lines.push_back({{}, text + 2 * offset});
        ++lines;
      }
    }
  }
  g_key_by_package = false;
  EOT_INFO("[text] {} translation: {} lines from package {:#x} ({} table(s))", g_language, lines, g_shipped->package,
           count);
  g_shipped = nullptr;
  g_shipped_object = 0;
  return lines > 0;
}

}

bool LoadTranslation(const std::filesystem::path &game, std::string_view language) {
  g_index.clear();
  g_lines.clear();
  g_language.assign(language);
  g_key_by_package = true;
  g_shipped = nullptr;
  g_shipped_object = 0;
  const Shipped *shipped = nullptr;
  for (const Shipped &s : kShipped)
    if (language == s.language)
      shipped = &s;
  const std::filesystem::path file = game / "Data" / "custom" / "lang" / (std::string(language) + ".tsv");
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    if (shipped) {
      g_shipped = shipped;
      EOT_INFO("[text] {} translation: from package {:#x} once it is mounted", language, shipped->package);
    }
    return false;
  }
  std::string row;
  size_t rows = 0, bad = 0;
  while (std::getline(in, row)) {
    if (!row.empty() && row.back() == '\r')
      row.pop_back();
    if (row.empty() || row[0] == '#')
      continue;
    std::vector<std::string_view> cols;
    size_t from = 0;
    for (size_t i = 0; i < 8; ++i) {
      const size_t tab = row.find('\t', from);
      if (tab == std::string::npos)
        break;
      cols.push_back(std::string_view(row).substr(from, tab - from));
      from = tab + 1;
    }
    if (cols.size() != 8) {
      ++bad;
      continue;
    }
    const uint32_t package = static_cast<uint32_t>(std::strtoul(std::string(cols[0]).c_str(), nullptr, 10));
    const uint32_t line = static_cast<uint32_t>(std::strtoul(std::string(cols[4]).c_str(), nullptr, 10));
    const uint32_t crc = static_cast<uint32_t>(std::strtoul(std::string(cols[5]).c_str(), nullptr, 16));
    std::u16string text = Decode(std::string_view(row).substr(from));
    if (shipped && shipped->fold)
      for (char16_t &c : text)
        c = FoldCyrillic(c);
    g_index[Key(package, crc, line)] = static_cast<uint32_t>(g_lines.size());
    g_lines.push_back({std::move(text), 0});
    ++rows;
  }
  if (bad)
    EOT_WARN("[text] {}: {} malformed row(s) skipped", file.string(), bad);
  EOT_INFO("[text] {} translation: {} lines from {}", language, rows, file.string());
  return rows > 0;
}

void PackageMounted(uint32_t id, uint32_t package) {
  if (g_shipped && g_shipped->package == id && package) {
    g_shipped_object = package;
    IndexShipped();
  }
}

size_t TranslatedLines() { return g_lines.size(); }

}

REX_HOOK_RAW(eot_PKPackage_FindString) {
  using namespace eot::text;
  const uint32_t package = ctx.r3.u32;
  const uint32_t table = ctx.r4.u32;
  const uint32_t string = ctx.r5.u32;
  const uint32_t line = ctx.r6.u32;
  if (g_index.empty() && !(g_shipped_object && IndexShipped())) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  if (!package || g_block_failed) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  const uint32_t tables = eot::mem::load<uint32_t>(package + kPackageTables);
  if (!tables || table >= eot::mem::load<uint32_t>(package + kPackageTableCount)) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  const uint32_t record = tables + table * kTableRecordSize;
  const uint32_t entries = eot::mem::load<uint32_t>(record + kTableEntries);
  if (!entries || string >= eot::mem::load<uint32_t>(record + kTableEntryCount)) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  const uint32_t crc = eot::mem::load<uint32_t>(entries + string * kEntrySize);
  const uint32_t id = g_key_by_package ? eot::mem::load<uint32_t>(package + kPackageId) : 0;
  const auto it = g_index.find(Key(id, crc, line));
  if (it == g_index.end()) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  if (!g_lines[it->second].guest)
    CopyIntoGuest(ctx, base);
  const uint32_t guest = g_lines[it->second].guest;
  if (!guest) {
    __imp__eot_PKPackage_FindString(ctx, base);
    return;
  }
  ctx.r3.u32 = guest;
}
