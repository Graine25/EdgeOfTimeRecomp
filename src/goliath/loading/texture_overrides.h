#pragma once

#include <rex/hook.h>

namespace eot::loading {

void ApplyTextureOverrides(const PPCContext &ctx, uint8_t *base);

void TextureOverridesPackageMounted(const PPCContext &ctx, uint8_t *base);

bool TextureOverridesSettled();

}
