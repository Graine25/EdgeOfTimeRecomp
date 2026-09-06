#include <cstdint>
#include <cstring>
#include <string>

#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/ui/menu_handles.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_GEEngineMgr_LoadMainPackage);
REX_EXTERN(__imp__eot_MMMemoryMgr_Alloc);
REX_EXTERN(__imp__eot_PKPackageMgrBC_Load);
REX_EXTERN(__imp__eot_PKPackage_Mount); // PKPackage_Mount(package r3)
REX_EXTERN(__imp__eot_Stream_Open);
REX_EXTERN(__imp__eot_GLAPIResource_FindResourceFromCRC); // (type r3, nameCRC r4) -> resource or 0

namespace {

constexpr uint32_t kPackageMgr = 0x824C8DE8;
constexpr uint32_t kMgrRecords = 16392;
constexpr uint32_t kMgrPackages = 8;
constexpr uint32_t kRecordSize = 152;
constexpr uint32_t kRecName = 0;
constexpr uint32_t kRecNameSize = 128;
constexpr uint32_t kRecDepCount = 128;
constexpr uint32_t kRecDeps = 132;
constexpr uint32_t kRecFlags = 148;

uint32_t GuestAlloc(const PPCContext &ctx, uint8_t *base, uint32_t size) {
  PPCContext call = ctx;
  call.r3.u32 = size;
  call.r4.u32 = 16;
  call.r5.u32 = 0xFFFFFFFFu;
  call.r6.u32 = 0;
  __imp__eot_MMMemoryMgr_Alloc(call, base);
  return call.r3.u32;
}

uint32_t RegisterPackage(const PPCContext &ctx, uint8_t *base, uint32_t id, const char *name,
                         uint32_t dependency) {
  const uint32_t slot = kPackageMgr + kMgrRecords + id * 4;
  uint32_t record = eot::mem::load<uint32_t>(slot);
  if (record) {
    EOT_INFO("[pkg] package {} already registered at {:#x}", id, record);
    return record;
  }
  record = GuestAlloc(ctx, base, kRecordSize);
  if (!record) {
    EOT_WARN("[pkg] no guest memory for the {} record", name);
    return 0;
  }
  for (uint32_t i = 0; i < kRecordSize; ++i)
    eot::mem::store<uint8_t>(record + i, 0);
  const size_t len = std::strlen(name);
  for (size_t i = 0; i < len && i < kRecNameSize - 1; ++i)
    eot::mem::store<uint8_t>(record + kRecName + static_cast<uint32_t>(i), static_cast<uint8_t>(name[i]));
  eot::mem::store<uint32_t>(record + kRecDepCount, 1);
  eot::mem::store<uint16_t>(record + kRecDeps, static_cast<uint16_t>(dependency));
  eot::mem::store<uint32_t>(record + kRecFlags, 0);
  eot::mem::store<uint32_t>(slot, record);
  return record;
}

uint32_t LoadPackage(const PPCContext &ctx, uint8_t *base, uint32_t id) {
  PPCContext call = ctx;
  call.r3.u32 = kPackageMgr;
  call.r4.u32 = id;
  call.r5.u32 = 0xFFFFFFFFu;
  call.r6.u32 = 0;
  call.r7.u32 = 0;
  __imp__eot_PKPackageMgrBC_Load(call, base);
  return call.r3.u32;
}

constexpr const char *kReeotTextures[] = {"Reeot_PopUpBox", "Reeot_Font_TempusGothic", "Reeot_Font_SansaCon"};
constexpr uint32_t kResourceTypeTexture = 4;

std::string GuestString(uint32_t addr, uint32_t max = 160) {
  std::string s;
  for (uint32_t i = 0; addr && i < max; ++i) {
    const char c = static_cast<char>(eot::mem::load<uint8_t>(addr + i));
    if (!c)
      break;
    s.push_back(c);
  }
  return s;
}

}

REX_HOOK_RAW(eot_PKPackage_Mount) {
  const uint32_t package = ctx.r3.u32;
  const uint32_t id = package ? eot::mem::load<uint32_t>(package + 176) : 0;
  const uint32_t record = id < 4096 ? eot::mem::load<uint32_t>(kPackageMgr + kMgrRecords + id * 4) : 0;
  __imp__eot_PKPackage_Mount(ctx, base);
  EOT_DEBUG("[pkg] mount id {} '{}' -> flags {:#x} openFlags {:#x}", id, GuestString(record),
           package ? eot::mem::load<uint32_t>(package + 160) : 0u,
           package ? eot::mem::load<uint32_t>(package + 164) : 0u);
  if (id != eot::ui::kReeotPackageId)
    return;
  if (package) {
    const uint32_t flags = eot::mem::load<uint32_t>(package + 160);
    eot::mem::store<uint32_t>(package + 160, flags | 0x8);
    EOT_INFO("[pkg] {} activation requested (flags {:#x} -> {:#x})", eot::ui::kReeotPackageName, flags,
             flags | 0x8);
  }
  for (const char *name : kReeotTextures) {
    PPCContext call = ctx;
    call.r3.u32 = kResourceTypeTexture;
    call.r4.u32 = eot::ui::NameCrc(name);
    __imp__eot_GLAPIResource_FindResourceFromCRC(call, base);
    EOT_DEBUG("[pkg] texture {} (crc {:#010x}) -> resource {:#x}", name, eot::ui::NameCrc(name), call.r3.u32);
  }
}

REX_HOOK_RAW(eot_Stream_Open) {
  const std::string name = GuestString(ctx.r4.u32);
  const uint32_t mode = ctx.r5.u32;
  __imp__eot_Stream_Open(ctx, base);
  EOT_DEBUG("[pkg] open '{}' mode {:#x} -> {:#x}", name, mode, ctx.r3.u32);
}

REX_HOOK_RAW(eot_GEEngineMgr_LoadMainPackage) {
  __imp__eot_GEEngineMgr_LoadMainPackage(ctx, base);
  using namespace eot::ui;
  if (eot::mem::load<uint32_t>(kPackageMgr + kMgrPackages + kReeotPackageId * 4)) {
    EOT_INFO("[pkg] {} (id {:#x}) is already loaded", kReeotPackageName, kReeotPackageId);
    return;
  }
  if (!RegisterPackage(ctx, base, kReeotPackageId, kReeotPackageName, kReeotPackageDependency))
    return;
  const uint32_t package = LoadPackage(ctx, base, kReeotPackageId);
  EOT_INFO("[pkg] queued {} as package id {:#x}: object {:#x}, handles {:#010x}+", kReeotPackageName,
           kReeotPackageId, package, kReeotHandleBase);
}
