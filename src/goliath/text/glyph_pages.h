#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/hook.h>

namespace eot::text {

void NoteGlyphPackage(uint32_t package);

bool GlyphTableReady(const char *table);

bool InstallGlyphs(const PPCContext &ctx, uint8_t *base, uint32_t font_record, const char *table,
                   const char *font);

std::vector<std::string> GlyphTableLines(const char *table);

struct IconCell {
  uint8_t slot;
  float u0, v0, u1, v1;
  float aspect;
  float lift;
  uint8_t alias;
};

uint32_t InstallIconPage(const PPCContext &ctx, uint8_t *base, uint32_t font_record, uint32_t page, uint32_t texture,
                         const std::vector<IconCell> &cells);

}
