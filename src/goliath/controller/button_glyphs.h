#pragma once

#include <cstdint>

#include <rex/hook.h>

namespace eot::controller {

void ButtonGlyphsTick(const PPCContext &ctx, uint8_t *base);

bool KeyCapInstalled(uint8_t slot);
void GlyphBindsChanged();
void NoteButtonHelper(uint32_t object);
void NoteButtonHelperZone(uint32_t zone);

}
