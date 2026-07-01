#pragma once

#include <cstdint>
#include <vector>

#include "src/goliath_engine/gpu/renderer/guest_resources.h"

namespace plume {
struct RenderDevice;
struct RenderCommandList;
struct RenderDescriptorSet;
struct RenderPipelineLayout;
struct RenderShader;
}

namespace eot::render {

plume::RenderDevice* Device();

bool EnsureTextureSystem();
plume::RenderDescriptorSet* TextureSet();
plume::RenderDescriptorSet* SamplerSet();
plume::RenderDescriptorSet* Tex3DSet();
plume::RenderDescriptorSet* TexCubeSet();
plume::RenderDescriptorSet* Tex1DSet();
plume::RenderPipelineLayout* TexturedPipelineLayout();
plume::RenderPipelineLayout* GameLayout();
void GameCbvIndices(uint32_t& vs, uint32_t& ps, uint32_t& shared);

void BeginTextureFrame();
uint32_t GetOrCreateTextureIndex(plume::RenderCommandList* cmd, uint32_t guestAddr,
                                 const TextureFetch& fetch);

void RegisterVertexDeclaration(uint8_t* base, uint32_t pElems, uint32_t pDecl);
bool DeclElementsFor(uint8_t* base, uint32_t pDecl, std::vector<DeclElem>& out);

void RegisterShader(uint8_t* base, uint32_t pFunction, uint32_t objVA, bool isVS);
void DiagStreamShader(uint8_t* base, uint32_t streamPos, uint32_t objVA, bool isVS);
void SetCurrentVertexShaderObject(uint8_t* base, uint32_t device, uint32_t objVA);
void SetCurrentPixelShaderObject(uint8_t* base, uint32_t device, uint32_t objVA);
void ResolveShadersForDraw(uint8_t* base, uint32_t deviceVA);
plume::RenderShader* CurrentVertexShader();
plume::RenderShader* CurrentPixelShader();
const uint32_t* CurrentVsFetchLayout(uint32_t& count);
bool CurrentVsIsWindowSpace();
void SetVertexFormatSpecBits(uint32_t bits);

}
