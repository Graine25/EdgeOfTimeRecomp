#pragma once

#include <cstdint>

#include "gamelogic/ui/hud_api.h"

namespace eot::ui {

constexpr uint32_t kPromptA = 1;
constexpr uint32_t kPromptBack = 15;
constexpr uint32_t kPromptY = 20;
constexpr uint32_t kPromptX = 27;

void OverridePrompt(uint32_t id, const char *name);
void RestorePrompt(uint32_t id);

void SendPromptMask(const PPCContext &ctx, uint8_t *base, uint32_t zone, uint32_t mask, uint32_t scratch);

}
