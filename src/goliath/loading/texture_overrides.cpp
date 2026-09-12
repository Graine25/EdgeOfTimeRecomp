#include "goliath/loading/texture_overrides.h"

#include <cstdint>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/ui/name_crc.h"

REX_EXTERN(__imp__eot_GLAPIResource_FindResourceFromCRC);      // (type r3, nameCRC r4) -> handle or -1
REX_EXTERN(__imp__eot_RZResourceMgrBC_GetResourceByHandle);    // (handle r3) -> record, referenced
REX_EXTERN(__imp__eot_RZResource_Release);                     // (record r3)
REX_EXTERN(__imp__eot_RZResourceMgrBC_AcquireDiscardableData); // (record r3, level r4, ownThread r5)
REX_EXTERN(__imp__eot_RZTexture_GetTexture);                   // (record r3) -> descriptor or 0
REX_EXTERN(__imp__eot_RZTexture_TextureReplace);               // (record r3, replacement r4)
REX_EXTERN(__imp__eot_PKPackageMgrBC_Update);

REXCVAR_DEFINE_BOOL(eot_texture_overrides, true, "EdgeOfTime/Config",
                    "Replace retail textures with the port's own art from its package.");

namespace eot::loading {
namespace {

constexpr uint32_t kTypeTexture = 4;
constexpr uint32_t kTypeFont = 7;
constexpr uint32_t kNoHandle = 0xFFFFFFFFu;
constexpr uint32_t kResidencyWord = 72;
constexpr uint32_t kRefBlockBytes = 512;

struct Override {
  const char *retail;
  const char *replacement;
  const char *font;
  uint32_t retailCrc;
  uint32_t replacementCrc;
  uint32_t fontCrc;
};

constexpr Override Make(const char *retail, const char *replacement, const char *font) {
  return {retail, replacement, font, eot::ui::NameCrc(retail), eot::ui::NameCrc(replacement),
          font ? eot::ui::NameCrc(font) : 0u};
}

constexpr Override kOverrides[] = {
    Make("TempusGothic_texture_0", "Reeot_Font_TempusGothic_4x", "TempusGothic"),
    Make("SansaCon-UltraBlack_texture_0", "Reeot_Font_SansaCon_4x", "SansaCon"),
};
constexpr uint32_t kOverrideCount = sizeof(kOverrides) / sizeof(kOverrides[0]);
static_assert(kOverrideCount <= 32, "the pending masks are 32 bits wide");

uint32_t g_pending = (1u << kOverrideCount) - 1;
uint32_t g_requested = 0;
uint32_t g_ticks = 0;
constexpr uint32_t kMaxTicks = 6000;

uint32_t Find(const PPCContext &ctx, uint8_t *base, uint32_t type, uint32_t crc) {
  PPCContext call = ctx;
  call.r3.u32 = type;
  call.r4.u32 = crc;
  __imp__eot_GLAPIResource_FindResourceFromCRC(call, base);
  return call.r3.u32 == kNoHandle ? 0 : call.r3.u32;
}

uint32_t Acquire(const PPCContext &ctx, uint8_t *base, uint32_t handle) {
  if (!handle)
    return 0;
  PPCContext call = ctx;
  call.r3.u32 = handle;
  __imp__eot_RZResourceMgrBC_GetResourceByHandle(call, base);
  return call.r3.u32;
}

void Release(const PPCContext &ctx, uint8_t *base, uint32_t record) {
  if (!record)
    return;
  PPCContext call = ctx;
  call.r3.u32 = record;
  __imp__eot_RZResource_Release(call, base);
}

uint32_t Descriptor(const PPCContext &ctx, uint8_t *base, uint32_t record) {
  PPCContext call = ctx;
  call.r3.u32 = record;
  __imp__eot_RZTexture_GetTexture(call, base);
  return call.r3.u32;
}

bool Resident(uint32_t record) { return (eot::mem::load<uint32_t>(record + kResidencyWord) & 1) != 0; }

void RequestLoad(const PPCContext &ctx, uint8_t *base, uint32_t record) {
  PPCContext call = ctx;
  call.r3.u32 = record;
  call.r4.u32 = 1;
  call.r5.u32 = 1;
  __imp__eot_RZResourceMgrBC_AcquireDiscardableData(call, base);
}

void RepointFontAtlas(const PPCContext &ctx, uint8_t *base, const Override &o, uint32_t retail,
                      uint32_t replacement) {
  const uint32_t font = Acquire(ctx, base, Find(ctx, base, kTypeFont, o.fontCrc));
  if (!font)
    return;
  uint32_t patched = 0;
  const auto rewrite = [&](uint32_t block) {
    for (uint32_t off = 0; off < kRefBlockBytes; off += 4) {
      const uint32_t v = eot::mem::load<uint32_t>(block + off);
      if (v == o.retailCrc) {
        eot::mem::store<uint32_t>(block + off, o.replacementCrc);
        ++patched;
      } else if (v == retail) {
        eot::mem::store<uint32_t>(block + off, replacement);
        ++patched;
      }
    }
  };
  rewrite(font);
  for (uint32_t off = 0; off < 256; off += 4) {
    const uint32_t ptr = eot::mem::load<uint32_t>(font + off);
    if (ptr >= 0xE0000000u && ptr < 0xF0000000u)
      rewrite(ptr);
  }
  Release(ctx, base, font);
  EOT_INFO("[tex] {}'s atlas reference now names {} ({} word(s))", o.font, o.replacement, patched);
}

bool TryApply(const PPCContext &ctx, uint8_t *base, uint32_t i) {
  const Override &o = kOverrides[i];
  const uint32_t retailHandle = Find(ctx, base, kTypeTexture, o.retailCrc);
  const uint32_t replacement = Acquire(ctx, base, Find(ctx, base, kTypeTexture, o.replacementCrc));
  if (!retailHandle || !replacement) {
    Release(ctx, base, replacement);
    return false;
  }

  const uint32_t descriptor = Resident(replacement) ? Descriptor(ctx, base, replacement) : 0;
  if (!descriptor) {
    if (!(g_requested & (1u << i))) {
      g_requested |= 1u << i;
      RequestLoad(ctx, base, replacement);
      EOT_INFO("[tex] {} asked to load; the swap waits for its data", o.replacement);
    }
    Release(ctx, base, replacement);
    return false;
  }

  const uint32_t retail = Acquire(ctx, base, retailHandle);
  if (retail) {
    PPCContext call = ctx;
    call.r3.u32 = retail;
    call.r4.u32 = replacement;
    __imp__eot_RZTexture_TextureReplace(call, base);
    EOT_INFO("[tex] {} -> {} ({:#x} -> {:#x}, descriptor {:#x})", o.retail, o.replacement, retail, replacement,
             descriptor);
    if (o.font)
      RepointFontAtlas(ctx, base, o, retail, replacement);
  }
  Release(ctx, base, retail);
  Release(ctx, base, replacement);
  return retail != 0;
}

}

void ApplyTextureOverrides(const PPCContext &ctx, uint8_t *base) {
  if (!REXCVAR_GET(eot_texture_overrides)) {
    g_pending = 0;
    return;
  }
  for (uint32_t i = 0; i < kOverrideCount; ++i)
    if ((g_pending & (1u << i)) && TryApply(ctx, base, i))
      g_pending &= ~(1u << i);
}

}

REX_HOOK_RAW(eot_PKPackageMgrBC_Update) {
  __imp__eot_PKPackageMgrBC_Update(ctx, base);
  using namespace eot::loading;
  if (!g_pending)
    return;
  if (++g_ticks > kMaxTicks) {
    EOT_WARN("[tex] {} override(s) never became ready; giving up", __builtin_popcount(g_pending));
    g_pending = 0;
    return;
  }
  ApplyTextureOverrides(ctx, base);
  if (!g_pending)
    EOT_DEBUG("[tex] overrides in place after {} manager ticks", g_ticks);
}
