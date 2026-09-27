#include "goliath/loading/texture_overrides.h"

#include <cstdint>
#include <mutex>
#include <string>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "goliath/controller/button_glyphs.h"
#include "goliath/loading/resources.h"
#include "goliath/text/glyph_pages.h"
#include "goliath/ui/name_crc.h"
#include "platform/language.h"

REX_EXTERN(__imp__eot_RZTexture_TextureReplace); // (record r3, replacement r4)
REX_EXTERN(__imp__eot_PKPackageMgrBC_Update);

REXCVAR_DEFINE_BOOL(eot_texture_overrides, true, "EdgeOfTime/Config",
                    "Replace retail textures with the port's own art from its package.");
REXCVAR_DEFINE_BOOL(eot_antivenom_normals, true, "EdgeOfTime/Config",
                    "Use the remastered normal maps on Anti-Venom as well as its diffuse ones.");

namespace eot::loading {
namespace {

constexpr uint32_t kRefBlockBytes = 512;

struct Override {
  const char *retail;
  const char *replacement;
  const char *font;
  const char *glyphs;
  const char *language;
  const char *cvar;
  uint32_t retailCrc;
  uint32_t replacementCrc;
  uint32_t fontCrc;
};

constexpr Override Make(const char *retail, const char *replacement, const char *font, const char *glyphs = nullptr,
                        const char *language = nullptr, const char *cvar = nullptr) {
  return {retail, replacement,
          font,   glyphs,
          language, cvar,
          eot::ui::NameCrc(retail), eot::ui::NameCrc(replacement),
          font ? eot::ui::NameCrc(font) : 0u};
}

constexpr Override kOverrides[] = {
    Make("TempusGothic_texture_0", "Reeot_Font_TempusGothic_4x", "TempusGothic", "GlyphsTempusGothic"),
    Make("SansaCon-UltraBlack_texture_0", "Reeot_Font_SansaCon_4x", "SansaCon", "GlyphsSansaCon"),
    Make("SMA_SMAmazingState00_D", "Reeot_SMA_State00_D", nullptr),
    Make("SMA_SMAmazingState00_N", "Reeot_SMA_State00_N", nullptr),
    Make("SMA_SMAmazingState00_S", "Reeot_SMA_State00_S", nullptr),
    Make("SMA_SMAmazingState03_D", "Reeot_SMA_State03_D", nullptr),
    Make("SMA_SMAmazingState03_N", "Reeot_SMA_State03_N", nullptr),
    Make("SMA_SMAmazingState03_S", "Reeot_SMA_State03_S", nullptr),
    Make("SMA_SMAmazingState05_D", "Reeot_SMA_State05_D", nullptr),
    Make("SMA_SMAmazingState05_N", "Reeot_SMA_State05_N", nullptr),
    Make("SMA_SMAmazingState05_S", "Reeot_SMA_State05_S", nullptr),
    Make("SMA_SMAmazingState07_D", "Reeot_SMA_State07_D", nullptr),
    Make("SMA_SMAmazingState07_N", "Reeot_SMA_State07_N", nullptr),
    Make("SMA_SMAmazingState07_S", "Reeot_SMA_State07_S", nullptr),
    Make("SMA_AntiVenom_D", "Reeot_AntiVenom_D", nullptr),
    Make("SMA_AntiVenom_N", "Reeot_AntiVenom_N", nullptr, nullptr, nullptr, "eot_antivenom_normals"),
    Make("SMA_MassiveAntiVenom_D", "Reeot_AntiVenomMassive_D", nullptr),
    Make("SMA_MassiveAntiVenom_N", "Reeot_AntiVenomMassive_N", nullptr, nullptr, nullptr,
         "eot_antivenom_normals"),
    Make("SM99_Spiderman_D", "Reeot_SM2099Body_D", nullptr),
    Make("SM99_Spiderman_N", "Reeot_SM2099Body_N", nullptr),
    Make("SM99_Spiderman_S", "Reeot_SM2099Body_S", nullptr),
};
constexpr uint32_t kOverrideCount = sizeof(kOverrides) / sizeof(kOverrides[0]);
static_assert(kOverrideCount <= 32, "the pending masks are 32 bits wide");

constexpr uint32_t FontMask() {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < kOverrideCount; ++i)
    if (kOverrides[i].font)
      mask |= 1u << i;
  return mask;
}
constexpr uint32_t kFontMask = FontMask();

uint32_t WantedMask() {
  const std::string tag = eot::platform::TranslationTag(rex::cvar::GetFlagByName("eot_language"));
  uint32_t mask = 0;
  for (uint32_t i = 0; i < kOverrideCount; ++i) {
    const Override &o = kOverrides[i];
    if (o.cvar && !rex::cvar::Query<bool>(o.cvar))
      continue;
    if (o.language) {
      if (tag == o.language)
        mask |= 1u << i;
      continue;
    }
    bool specific = false;
    for (uint32_t k = 0; k < kOverrideCount; ++k)
      specific = specific || (kOverrides[k].language && tag == kOverrides[k].language &&
                              kOverrides[k].retailCrc == o.retailCrc);
    if (!specific)
      mask |= 1u << i;
  }
  return mask;
}

uint32_t g_pending = 0;
uint32_t g_wanted = 0;
bool g_pending_chosen = false;
std::recursive_mutex g_apply_mutex;
uint32_t g_requested = 0;
uint32_t g_waiting_data = 0;
uint32_t g_swapped[kOverrideCount] = {};
uint32_t g_ticks = 0;
constexpr uint32_t kMaxTicks = 6000;

void RepointFontAtlas(const PPCContext &ctx, uint8_t *base, const Override &o, uint32_t retail,
                      uint32_t replacement) {
  const uint32_t font = AcquireResource(ctx, base, FindResource(ctx, base, kTypeFont, o.fontCrc));
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
  if (o.glyphs)
    eot::text::InstallGlyphs(ctx, base, font, o.glyphs, o.font);
  ReleaseResource(ctx, base, font);
  EOT_INFO("[tex] {}'s atlas reference now names {} ({} word(s))", o.font, o.replacement, patched);
}

enum class Try { Applied, NoRetail, NotReady };

Try TryApply(const PPCContext &ctx, uint8_t *base, uint32_t i) {
  const Override &o = kOverrides[i];
  const uint32_t retailHandle = FindResource(ctx, base, kTypeTexture, o.retailCrc);
  if (!retailHandle)
    return Try::NoRetail;
  const uint32_t replacement = AcquireResource(ctx, base, FindResource(ctx, base, kTypeTexture, o.replacementCrc));
  if (!replacement)
    return Try::NotReady;

  const uint32_t descriptor = ResourceResident(replacement) ? TextureDescriptor(ctx, base, replacement) : 0;
  if (!descriptor) {
    if (!(g_requested & (1u << i))) {
      g_requested |= 1u << i;
      RequestResourceLoad(ctx, base, replacement);
      EOT_INFO("[tex] {} asked to load; the swap waits for its data", o.replacement);
    }
    ReleaseResource(ctx, base, replacement);
    return Try::NotReady;
  }
  if (o.glyphs && !eot::text::GlyphTableReady(o.glyphs)) {
    ReleaseResource(ctx, base, replacement);
    return Try::NotReady;
  }

  const uint32_t retail = AcquireResource(ctx, base, retailHandle);
  if (retail) {
    PPCContext call = ctx;
    call.r3.u32 = retail;
    call.r4.u32 = replacement;
    __imp__eot_RZTexture_TextureReplace(call, base);
    EOT_INFO("[tex] {} -> {} ({:#x} -> {:#x}, descriptor {:#x})", o.retail, o.replacement, retail, replacement,
             descriptor);
    if (o.font)
      RepointFontAtlas(ctx, base, o, retail, replacement);
    g_swapped[i] = retailHandle;
  }
  ReleaseResource(ctx, base, retail);
  ReleaseResource(ctx, base, replacement);
  return retail ? Try::Applied : Try::NotReady;
}

}

void ApplyTextureOverrides(const PPCContext &ctx, uint8_t *base) {
  std::lock_guard<std::recursive_mutex> lock(g_apply_mutex);
  if (!g_pending_chosen) {
    g_pending_chosen = true;
    g_wanted = REXCVAR_GET(eot_texture_overrides) ? WantedMask() : 0;
    g_pending = g_wanted;
    EOT_INFO("[tex] overrides wanted: {:#x} of {}", g_pending, kOverrideCount);
  }
  if (!g_pending)
    return;
  g_waiting_data = 0;
  for (uint32_t i = 0; i < kOverrideCount; ++i) {
    if (!(g_pending & (1u << i)))
      continue;
    switch (TryApply(ctx, base, i)) {
    case Try::Applied:
      g_pending &= ~(1u << i);
      break;
    case Try::NotReady:
      g_waiting_data |= 1u << i;
      break;
    case Try::NoRetail:
      break;
    }
  }
}

void TextureOverridesPackageMounted(const PPCContext &ctx, uint8_t *base) {
  std::lock_guard<std::recursive_mutex> lock(g_apply_mutex);
  if (!g_pending_chosen)
    return;
  uint32_t again = 0;
  for (uint32_t i = 0; i < kOverrideCount; ++i) {
    if (!(g_wanted & (1u << i)) || (g_pending & (1u << i)) || kOverrides[i].font)
      continue;
    const uint32_t handle = FindResource(ctx, base, kTypeTexture, kOverrides[i].retailCrc);
    if (handle && handle != g_swapped[i])
      again |= 1u << i;
  }
  if (again) {
    EOT_INFO("[tex] {} retail texture(s) registered anew; swapping again", __builtin_popcount(again));
    g_pending |= again;
  }
  if (g_pending)
    ApplyTextureOverrides(ctx, base);
}

bool TextureOverridesSettled() { return g_pending_chosen && !(g_pending & kFontMask); }

}

REX_HOOK_RAW(eot_PKPackageMgrBC_Update) {
  __imp__eot_PKPackageMgrBC_Update(ctx, base);
  using namespace eot::loading;
  if (g_pending_chosen && g_pending) {
    std::lock_guard<std::recursive_mutex> lock(g_apply_mutex);
    ApplyTextureOverrides(ctx, base);
    if (g_waiting_data && ++g_ticks > kMaxTicks) {
      EOT_WARN("[tex] {} override(s) never became ready; giving up", __builtin_popcount(g_waiting_data));
      g_pending &= ~g_waiting_data;
      g_waiting_data = 0;
    }
    if (!g_pending)
      EOT_DEBUG("[tex] overrides in place after {} manager ticks", g_ticks);
  }
  eot::controller::ButtonGlyphsTick(ctx, base);
}
