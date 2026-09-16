#include "goliath/text/glyph_pages.h"

#include <bit>
#include <cstdio>
#include <string>
#include <vector>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_MMMemoryMgr_Alloc);

namespace eot::text {
namespace {

constexpr uint32_t kFontPages = 80;
constexpr uint32_t kFontPageCount = 88;
constexpr uint32_t kPageRecords = 256;
constexpr uint32_t kRecordSize = 40;
constexpr uint32_t kPageBytes = kPageRecords * kRecordSize;
constexpr uint32_t kRecTexture = 0;
constexpr uint32_t kRecU0 = 8;
constexpr uint32_t kRecV0 = 12;
constexpr uint32_t kRecU1 = 16;
constexpr uint32_t kRecV1 = 20;
constexpr uint32_t kRecAdvance = 24;
constexpr uint32_t kRecHeight = 28;
constexpr uint32_t kRecTop = 32;
constexpr uint32_t kRecFlags = 36;
constexpr uint32_t kFlagLive = 0x8000;
constexpr uint32_t kReference = 0x41;

constexpr uint32_t kPackageTables = 476;
constexpr uint32_t kPackageTableCount = 480;
constexpr uint32_t kTableRecordSize = 24;
constexpr uint32_t kTableEntries = 0;
constexpr uint32_t kTableText = 8;
constexpr uint32_t kTableEntryCount = 12;
constexpr uint32_t kTableNameCrc = 20;
constexpr uint32_t kEntrySize = 12;
constexpr uint32_t kEntryLines = 8;

constexpr float kUnitsX = 640.0f;
constexpr float kUnitsY = 480.0f;

std::vector<uint32_t> g_packages;
std::vector<uint32_t> g_done;

float LoadF(uint32_t at) { return std::bit_cast<float>(eot::mem::load<uint32_t>(at)); }
void StoreF(uint32_t at, float v) { eot::mem::store<uint32_t>(at, std::bit_cast<uint32_t>(v)); }

std::vector<std::string> TableLines(uint32_t package, uint32_t crc) {
  std::vector<std::string> lines;
  const uint32_t tables = eot::mem::load<uint32_t>(package + kPackageTables);
  const uint32_t count = eot::mem::load<uint32_t>(package + kPackageTableCount);
  for (uint32_t t = 0; tables && t < count; ++t) {
    const uint32_t record = tables + t * kTableRecordSize;
    if (eot::mem::load<uint32_t>(record + kTableNameCrc) != crc)
      continue;
    const uint32_t entries = eot::mem::load<uint32_t>(record + kTableEntries);
    const uint32_t text = eot::mem::load<uint32_t>(record + kTableText);
    const uint32_t n = eot::mem::load<uint32_t>(record + kTableEntryCount);
    for (uint32_t s = 0; entries && text && s < n; ++s) {
      const uint32_t lineRecords = eot::mem::load<uint32_t>(entries + s * kEntrySize + kEntryLines);
      uint32_t at = text + 2 * eot::mem::load<uint32_t>(lineRecords);
      std::string line;
      for (uint32_t k = 0; k < 96; ++k, at += 2) {
        const uint16_t c = eot::mem::load<uint16_t>(at);
        if (!c)
          break;
        line.push_back(c < 0x80 ? static_cast<char>(c) : '?');
      }
      lines.push_back(std::move(line));
    }
    break;
  }
  return lines;
}

std::vector<std::string> TableLines(const char *table) {
  const uint32_t crc = eot::ui::NameCrc(table);
  for (const uint32_t package : g_packages) {
    std::vector<std::string> lines = TableLines(package, crc);
    if (!lines.empty())
      return lines;
  }
  return {};
}

uint32_t Page(uint32_t font, uint32_t cp) {
  if ((cp >> 8) >= eot::mem::load<uint32_t>(font + kFontPageCount))
    return 0;
  return eot::mem::load<uint32_t>(eot::mem::load<uint32_t>(font + kFontPages) + (cp >> 8) * 4);
}

}

void NoteGlyphPackage(uint32_t package) {
  if (!package)
    return;
  for (const uint32_t p : g_packages)
    if (p == package)
      return;
  g_packages.push_back(package);
}

bool GlyphTableReady(const char *table) { return !TableLines(table).empty(); }

std::vector<std::string> GlyphTableLines(const char *table) { return TableLines(table); }

uint32_t InstallIconPage(const PPCContext &ctx, uint8_t *base, uint32_t font, uint32_t page, uint32_t texture,
                         const std::vector<IconCell> &cells) {
  constexpr uint32_t kButtons = 3;
  const uint32_t pages = eot::mem::load<uint32_t>(font + kFontPages);
  const uint32_t pageCount = eot::mem::load<uint32_t>(font + kFontPageCount);
  const uint32_t buttons = Page(font, kButtons << 8);
  if (!pages || !buttons || page >= pageCount || page <= kButtons)
    return 0;
  uint32_t at = eot::mem::load<uint32_t>(pages + page * 4);
  if (!at) {
    PPCContext call = ctx;
    call.r3.u32 = kPageBytes;
    call.r4.u32 = 16;
    call.r5.u32 = 0xFFFFFFFFu;
    call.r6.u32 = 0;
    __imp__eot_MMMemoryMgr_Alloc(call, base);
    at = call.r3.u32;
    if (!at)
      return 0;
    eot::mem::store<uint32_t>(pages + page * 4, at);
  }
  for (uint32_t off = 0; off < kPageBytes; off += 4)
    eot::mem::store<uint32_t>(at + off, 0);
  for (const IconCell &cell : cells) {
    const uint32_t src = buttons + cell.slot * kRecordSize;
    if (!(eot::mem::load<uint16_t>(src + kRecFlags + 2) & kFlagLive))
      continue;
    const uint32_t rec = at + cell.slot * kRecordSize;
    if (cell.alias != 0xFF) {
      const uint32_t other = buttons + cell.alias * kRecordSize;
      if (eot::mem::load<uint16_t>(other + kRecFlags + 2) & kFlagLive)
        for (uint32_t off = 0; off < kRecordSize; off += 4)
          eot::mem::store<uint32_t>(rec + off, eot::mem::load<uint32_t>(other + off));
      continue;
    }
    const float height = LoadF(src + kRecHeight);
    eot::mem::store<uint32_t>(rec + kRecTexture, texture);
    eot::mem::store<uint32_t>(rec + 4, 0);
    StoreF(rec + kRecU0, cell.u0);
    StoreF(rec + kRecV0, cell.v0);
    StoreF(rec + kRecU1, cell.u1);
    StoreF(rec + kRecV1, cell.v1);
    StoreF(rec + kRecAdvance, cell.aspect > 0 ? height * (kUnitsY / kUnitsX) * cell.aspect : LoadF(src + kRecAdvance));
    StoreF(rec + kRecHeight, height);
    StoreF(rec + kRecTop, LoadF(src + kRecTop) + cell.lift / kUnitsY);
    eot::mem::store<uint32_t>(rec + kRecFlags, kFlagLive);
  }
  return at;
}

bool InstallGlyphs(const PPCContext &ctx, uint8_t *base, uint32_t font, const char *table, const char *name) {
  for (const uint32_t done : g_done)
    if (done == font)
      return true;
  const std::vector<std::string> lines = TableLines(table);
  if (lines.empty()) {
    EOT_WARN("[glyphs] no {} table in any mounted package yet; {} keeps its Latin-1", table, name);
    return false;
  }
  const uint32_t pages = eot::mem::load<uint32_t>(font + kFontPages);
  const uint32_t pageCount = eot::mem::load<uint32_t>(font + kFontPageCount);
  const uint32_t page0 = Page(font, 0);
  if (!pages || !page0 || pageCount <= 4) {
    EOT_WARN("[glyphs] {}: no page table to extend ({} pages)", name, pageCount);
    return false;
  }
  if (eot::mem::load<uint32_t>(pages + 4 * 4)) {
    g_done.push_back(font);
    return true;
  }
  const uint32_t reference = page0 + kReference * kRecordSize;
  const float refTop = LoadF(reference + kRecTop);

  float width = 0, oldHeight = 0, newHeight = 0;
  uint32_t page = 0, cells = 0, aliases = 0;
  const float refRows = (LoadF(reference + kRecV1) - LoadF(reference + kRecV0));
  for (const std::string &line : lines) {
    float a = 0, b = 0, c = 0, d = 0, e = 0;
    unsigned cp = 0, target = 0;
    if (std::sscanf(line.c_str(), "size %f %f %f", &a, &b, &c) == 3) {
      width = a;
      oldHeight = b;
      newHeight = c;
      if (width <= 0 || oldHeight <= 0 || newHeight <= 0)
        return false;
      const float scale = oldHeight / newHeight;
      const uint32_t atlas = eot::mem::load<uint32_t>(reference + kRecTexture);
      for (uint32_t p = 0; p < pageCount; ++p) {
        const uint32_t pg = eot::mem::load<uint32_t>(pages + p * 4);
        for (uint32_t i = 0; pg && i < kPageRecords; ++i) {
          const uint32_t rec = pg + i * kRecordSize;
          if (!(eot::mem::load<uint16_t>(rec + kRecFlags + 2) & kFlagLive))
            continue;
          if (eot::mem::load<uint32_t>(rec + kRecTexture) != atlas)
            continue;
          StoreF(rec + kRecV0, LoadF(rec + kRecV0) * scale);
          StoreF(rec + kRecV1, LoadF(rec + kRecV1) * scale);
        }
      }
      PPCContext call = ctx;
      call.r3.u32 = kPageBytes;
      call.r4.u32 = 16;
      call.r5.u32 = 0xFFFFFFFFu;
      call.r6.u32 = 0;
      __imp__eot_MMMemoryMgr_Alloc(call, base);
      page = call.r3.u32;
      if (!page) {
        EOT_WARN("[glyphs] {}: no guest memory for the Cyrillic page", name);
        return false;
      }
      for (uint32_t off = 0; off < kPageBytes; off += 4)
        eot::mem::store<uint32_t>(page + off, 0);
      eot::mem::store<uint32_t>(pages + 4 * 4, page);
      continue;
    }
    if (!page)
      continue;
    if (std::sscanf(line.c_str(), "cell %x %f %f %f %f %f", &cp, &a, &b, &c, &d, &e) == 6) {
      if ((cp >> 8) != 4)
        continue;
      const uint32_t rec = page + (cp & 0xFF) * kRecordSize;
      eot::mem::store<uint32_t>(rec + kRecTexture, eot::mem::load<uint32_t>(reference + kRecTexture));
      eot::mem::store<uint32_t>(rec + 4, 0);
      StoreF(rec + kRecU0, a / width);
      StoreF(rec + kRecV0, b / newHeight);
      StoreF(rec + kRecU1, c / width);
      StoreF(rec + kRecV1, d / newHeight);
      StoreF(rec + kRecAdvance, (c - a) / kUnitsX);
      StoreF(rec + kRecHeight, (d - b) / kUnitsY);
      StoreF(rec + kRecTop, refTop + (refRows * oldHeight - e) / kUnitsY);
      eot::mem::store<uint32_t>(rec + kRecFlags, kFlagLive);
      ++cells;
      continue;
    }
    if (std::sscanf(line.c_str(), "alias %x %x", &cp, &target) == 2) {
      const uint32_t from = Page(font, target);
      if ((cp >> 8) != 4 || !from)
        continue;
      const uint32_t src = from + (target & 0xFF) * kRecordSize;
      const uint32_t rec = page + (cp & 0xFF) * kRecordSize;
      for (uint32_t off = 0; off < kRecordSize; off += 4)
        eot::mem::store<uint32_t>(rec + off, eot::mem::load<uint32_t>(src + off));
      ++aliases;
    }
  }
  g_done.push_back(font);
  EOT_INFO("[glyphs] {}: Cyrillic page at {:#x}, {} cells and {} aliases; the sheet is {}x{} ({} rows retail)", name,
           page, cells, aliases, width, newHeight, oldHeight);
  return true;
}

}
