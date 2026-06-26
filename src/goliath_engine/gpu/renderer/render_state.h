#pragma once

#include <cstdint>

#include "src/goliath_engine/gpu/renderer/guest_resources.h"

namespace plume {
struct RenderCommandList;
}

namespace eot::render {

void SetStreamSource(uint8_t* base, uint32_t stream, uint32_t vbObject,
                     uint32_t offset, uint32_t stride);
void SetIndices(uint32_t ibObject);

void SetBoundTexture(uint32_t sampler, uint32_t addr, const TextureFetch& fetch);
bool GetBoundTexture(uint32_t sampler, uint32_t& addr, TextureFetch& fetch);

void DrawVertices(uint8_t* base, uint32_t device, uint32_t prim, uint32_t startVertex,
                  uint32_t vertexCount);
void BeginVertices(uint8_t* base, uint32_t device, uint32_t prim, uint32_t vertexCount,
                   uint32_t stride, uint32_t ringPtr);
void DrawIndexedVertices(uint8_t* base, uint32_t device, uint32_t prim, int32_t baseVertexIndex,
                         uint32_t startIndex, uint32_t indexCount);

void ReplayCapturedDraws(plume::RenderCommandList* cmd, uint32_t backbufferW,
                         uint32_t backbufferH);

void SubmitMovieFrame(uint8_t* base, uint32_t yuvVA);
bool PresentMovieFrame(plume::RenderCommandList* cmd, uint32_t backbufferW, uint32_t backbufferH);

}
