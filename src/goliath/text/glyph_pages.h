#pragma once

#include <cstdint>

#include <rex/hook.h>

namespace eot::text {

void SetGlyphPackage(uint32_t package);

bool ShippedGlyphsReady(const char *font);

bool InstallShippedGlyphs(const PPCContext &ctx, uint8_t *base, uint32_t font_record, const char *font);

}
