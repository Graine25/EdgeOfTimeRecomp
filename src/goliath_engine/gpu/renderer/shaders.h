#pragma once

#include <cstdint>

namespace plume {
struct RenderShader;
}

namespace eot::gpu {

plume::RenderShader* CurrentVertexShader();
plume::RenderShader* CurrentPixelShader();

bool CurrentVsIsWindowSpace();

void ResolveShadersForDraw(uint8_t* base, uint32_t deviceVA);

void SetVertexFormatSpecBits(uint32_t bits);

}
