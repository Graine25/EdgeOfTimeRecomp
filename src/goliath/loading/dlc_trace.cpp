#include "goliath/loading/dlc_trace.h"

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"

REX_EXTERN(__imp__eot_XContentCreate);

namespace {

constexpr uint32_t kGdlcCount = 0x8249BCE0;
constexpr uint32_t kGdlcObjects = 0x8249BCE8;
constexpr uint32_t kGdlcMaxObjects = 16;
constexpr uint32_t kObjEntries = 0;
constexpr uint32_t kObjEntryCount = 256;
constexpr uint32_t kObjFlags = 2352;
constexpr uint32_t kObjSlot = 2364;
constexpr uint32_t kObjFileName = 2400;
constexpr uint32_t kObjOpened = 2448;
constexpr uint32_t kEntryId = 0;               // u32 read at file offset 0x18
constexpr uint32_t kEntryPath = 4;
constexpr uint32_t kXContentFileName = 0x108;

constexpr uint32_t kCostumeTable = 0x883DD878;
constexpr uint32_t kCostumeCount = 0x883DDC38;
constexpr uint32_t kCostumeStride = 32;
constexpr uint32_t kCostumeHud = 28;
constexpr uint32_t kCostumeCapacity = 30;

std::string GuestString(uint32_t va, uint32_t max) {
  std::string s;
  for (uint32_t i = 0; va && i < max; ++i) {
    const char c = static_cast<char>(eot::mem::load<uint8_t>(va + i));
    if (!c)
      break;
    s.push_back(c);
  }
  return s;
}

std::string DescribeItem(uint32_t index) {
  const uint32_t obj = eot::mem::load<uint32_t>(kGdlcObjects + index * 4);
  if (!obj || !eot::mem::readable(obj, kObjOpened + 2))
    return {};
  const uint32_t flags = eot::mem::load<uint32_t>(obj + kObjFlags);
  const uint32_t count = eot::mem::load<uint32_t>(obj + kObjEntryCount);
  std::string line = std::format("item {} '{}' GDLC{}: flags {:#x} opened {} paks {}", index,
                                 GuestString(obj + kObjFileName, 42), eot::mem::load<uint32_t>(obj + kObjSlot),
                                 flags, eot::mem::load<uint8_t>(obj + kObjOpened), count);
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    const uint32_t entry = eot::mem::load<uint32_t>(obj + kObjEntries + i * 4);
    if (!entry)
      continue;
    line += std::format("{} id {:#x} '{}'", i ? "," : ":", eot::mem::load<uint32_t>(entry + kEntryId),
                        GuestString(entry + kEntryPath, 256));
  }
  return line;
}

std::vector<std::string> g_items;
std::string g_costumes;
bool g_gamelogic_mapped = false;

}

REX_HOOK_RAW(eot_XContentCreate) {
  const std::string root = GuestString(ctx.r4.u32, 32);
  const std::string file = ctx.r5.u32 ? GuestString(ctx.r5.u32 + kXContentFileName, 42) : std::string();
  __imp__eot_XContentCreate(ctx, base);
  EOT_INFO("[dlc] XContentCreate('{}', '{}') -> {:#x}", root, file, ctx.r3.u32);
}

namespace eot::loading {

void DlcTraceTick() {
  const uint32_t count = eot::mem::load<uint32_t>(kGdlcCount);
  if (count <= kGdlcMaxObjects) {
    if (g_items.size() < count)
      g_items.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      std::string line = DescribeItem(i);
      if (line != g_items[i]) {
        if (!line.empty())
          EOT_INFO("[dlc] {}", line);
        g_items[i] = std::move(line);
      }
    }
  }

  if (!g_gamelogic_mapped) {
    if (!eot::mem::readable(kCostumeTable, kCostumeStride * kCostumeCapacity) ||
        !eot::mem::readable(kCostumeCount, 4))
      return;
    g_gamelogic_mapped = true;
  }
  const uint32_t costumes = eot::mem::load<uint32_t>(kCostumeCount);
  std::string line;
  for (uint32_t i = 0; i < costumes && i < kCostumeCapacity; ++i) {
    const uint32_t row = kCostumeTable + i * kCostumeStride;
    line += std::format("{}{}{}", i ? " " : "", eot::mem::load<uint32_t>(row),
                        eot::mem::load<uint32_t>(row + kCostumeHud) ? "" : "(no gallery row)");
  }
  if (line != g_costumes) {
    EOT_INFO("[dlc] costume table: {} entries{}{}", costumes, line.empty() ? "" : ": ids ", line);
    g_costumes = std::move(line);
  }
}

}
