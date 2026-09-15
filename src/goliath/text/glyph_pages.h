#pragma once

#include <cstdint>

#include <rex/hook.h>

namespace eot::text {

void NoteGlyphPackage(uint32_t package);

bool GlyphTableReady(const char *table);

bool InstallGlyphs(const PPCContext &ctx, uint8_t *base, uint32_t font_record, const char *table,
                   const char *font);

}
