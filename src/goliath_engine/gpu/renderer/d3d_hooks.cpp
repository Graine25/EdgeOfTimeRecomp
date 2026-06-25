#include <rex/hook.h>
#include <rex/logging.h>

#include <bit>
#include <cstdint>

#include "src/goliath_engine/gpu/renderer/render_internal.h"
#include "src/goliath_engine/gpu/renderer/render_state.h"
#include "src/goliath_engine/gpu/renderer/video.h"
#include "src/goliath_engine/kernel/guest_memory.h"

namespace gmem = eot::kernel::memory;

namespace {
float ReadGuestF32(uint8_t* base, uint32_t va) {
  return std::bit_cast<float>(gmem::ReadU32(base, va));
}
}

REX_EXTERN(__imp__D3DDevice_ClearF);
REX_HOOK_RAW(D3DDevice_ClearF) {
  const uint32_t flags = ctx.r4.u32;
  const uint32_t pColor = ctx.r6.u32;
  bool haveColor = false;
  float r = 0, g = 0, b = 0, a = 1;
  if ((flags & 0x1) && pColor >= 0x1000) {
    r = ReadGuestF32(base, pColor + 0);
    g = ReadGuestF32(base, pColor + 4);
    b = ReadGuestF32(base, pColor + 8);
    a = ReadGuestF32(base, pColor + 12);
    haveColor = true;
  }

  __imp__D3DDevice_ClearF(ctx, base);

  if (haveColor) {
    Video::SetClearColor(r, g, b, a);
    static int s_logged = 0;
    if (s_logged < 4) {
      ++s_logged;
      REXGPU_INFO("[clear] flags=0x{:X} color=({:.3f}, {:.3f}, {:.3f}, {:.3f})", flags, r, g, b, a);
    }
  }
}

REX_EXTERN(__imp__D3DDevice_SetStreamSource);
REX_HOOK_RAW(D3DDevice_SetStreamSource) {
  eot::render::SetStreamSource(base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  __imp__D3DDevice_SetStreamSource(ctx, base);
}

REX_EXTERN(__imp__D3DDevice_SetIndices);
REX_HOOK_RAW(D3DDevice_SetIndices) {
  eot::render::SetIndices(ctx.r4.u32);
  __imp__D3DDevice_SetIndices(ctx, base);
}

REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_HOOK_RAW(D3DDevice_DrawVertices) {
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  __imp__D3DDevice_DrawVertices(ctx, base);
  eot::render::DrawVertices(base, device, prim, start, count);
}

REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_HOOK_RAW(D3DDevice_BeginVertices) {
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  __imp__D3DDevice_BeginVertices(ctx, base);
  const uint32_t ptr = ctx.r3.u32;
  eot::render::BeginVertices(base, device, prim, count, stride, ptr);
}

REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32;
  const int32_t baseVertex = static_cast<int32_t>(ctx.r5.u32);
  const uint32_t startIndex = ctx.r6.u32, indexCount = ctx.r7.u32;
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
  eot::render::DrawIndexedVertices(base, device, prim, baseVertex, startIndex, indexCount);
}

REX_EXTERN(__imp__D3DDevice_SetVertexShader);
REX_HOOK_RAW(D3DDevice_SetVertexShader) {
  const uint32_t device = ctx.r3.u32, obj = ctx.r4.u32;
  __imp__D3DDevice_SetVertexShader(ctx, base);
  eot::render::SetCurrentVertexShaderObject(base, device, obj);
}

REX_EXTERN(__imp__D3DDevice_SetPixelShader);
REX_HOOK_RAW(D3DDevice_SetPixelShader) {
  const uint32_t device = ctx.r3.u32, obj = ctx.r4.u32;
  __imp__D3DDevice_SetPixelShader(ctx, base);
  eot::render::SetCurrentPixelShaderObject(base, device, obj);
}

REX_EXTERN(__imp__D3DDevice_SetTexture);
REX_HOOK_RAW(D3DDevice_SetTexture) {
  const uint32_t sampler = ctx.r4.u32;
  const uint32_t tex = ctx.r5.u32;
  if (sampler < 16) {
    uint32_t addr = 0;
    eot::render::TextureFetch fetch;
    if (tex >= 0x1000) {
      addr = tex;
      for (uint32_t i = 0; i < 6; ++i)
        fetch.dword[i] = gmem::ReadU32(base, tex + 0x1C + i * 4);
    }
    eot::render::SetBoundTexture(sampler, addr, fetch);
  }
  __imp__D3DDevice_SetTexture(ctx, base);
}
