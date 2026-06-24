#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>

namespace {
struct Counters {
  std::atomic<uint64_t> createDevice{0}, setRenderTarget{0}, setDepthStencil{0}, clear{0},
      setViewport{0}, setVS{0}, setPS{0}, setVSConst{0}, setPSConst{0}, setTexture{0},
      setStream{0}, setIndices{0}, beginVertices{0}, drawVertices{0}, drawIndexed{0};
};
Counters g;
uint64_t take(std::atomic<uint64_t>& c) { return c.exchange(0, std::memory_order_relaxed); }
}

#define OBSERVE(NAME, FIELD)                                   \
  REX_EXTERN(__imp__##NAME);                                   \
  REX_HOOK_RAW(NAME) {                                         \
    g.FIELD.fetch_add(1, std::memory_order_relaxed);          \
    __imp__##NAME(ctx, base);                                  \
  }

OBSERVE(Direct3D_CreateDevice, createDevice)
OBSERVE(D3DDevice_SetRenderTarget, setRenderTarget)
OBSERVE(D3DDevice_SetDepthStencilSurface, setDepthStencil)
OBSERVE(D3DDevice_SetViewport, setViewport)
OBSERVE(D3DDevice_SetVertexShaderConstantFN, setVSConst)
OBSERVE(D3DDevice_SetPixelShaderConstantFN, setPSConst)

REX_EXTERN(__imp__Engine_Present);
REX_HOOK_RAW(Engine_Present) {
  static std::atomic<uint64_t> s_frames{0};
  uint64_t f = s_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((f % 120) == 0) {
    REXGPU_INFO(
        "[observe] frame {} | /120f: CreateDevice={} SetRT={} SetDS={} Clear={} Vport={} "
        "SetVS={} SetPS={} VSConst={} PSConst={} SetTex={} SetStream={} SetIdx={} BeginVtx={} "
        "DrawVtx={} DrawIndexed={}",
        f, take(g.createDevice), take(g.setRenderTarget), take(g.setDepthStencil), take(g.clear),
        take(g.setViewport), take(g.setVS), take(g.setPS), take(g.setVSConst), take(g.setPSConst),
        take(g.setTexture), take(g.setStream), take(g.setIndices), take(g.beginVertices),
        take(g.drawVertices), take(g.drawIndexed));
  }
  __imp__Engine_Present(ctx, base);
}
