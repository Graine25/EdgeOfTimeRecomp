#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>

#include "src/goliath_engine/kernel/guest_memory.h"

namespace gmem = eot::kernel::memory;

namespace {
std::atomic<uint64_t> g_declCount{0};

const char* UsageName(uint32_t u) {
  switch (u) {
    case 0: return "POSITION"; case 1: return "BLENDWEIGHT"; case 2: return "BLENDINDICES";
    case 3: return "NORMAL"; case 4: return "PSIZE"; case 5: return "TEXCOORD";
    case 6: return "TANGENT"; case 7: return "BINORMAL"; case 8: return "TESSFACTOR";
    case 9: return "POSITIONT"; case 10: return "COLOR"; case 11: return "FOG";
    case 12: return "DEPTH"; case 13: return "SAMPLE"; default: return "?";
  }
}
}

REX_EXTERN(__imp__XGSetVertexDeclaration);
REX_HOOK_RAW(XGSetVertexDeclaration) {
  const uint32_t pElems = ctx.r3.u32;
  const uint32_t pDecl = ctx.r4.u32;
  uint64_t n = g_declCount.fetch_add(1, std::memory_order_relaxed);
  if (n < 8 && pElems >= 0x1000) {
    REXGPU_INFO("[vtx] DECL #{} elems=0x{:08X} -> decl=0x{:08X}", n, pElems, pDecl);
    for (uint32_t i = 0; i < 32; ++i) {
      const uint32_t e = pElems + i * 12;  // sizeof(D3DVERTEXELEMENT9)
      const uint32_t d0 = gmem::ReadU32(base, e + 0);  // Stream<<16 | Offset
      const uint16_t stream = static_cast<uint16_t>(d0 >> 16);
      if (stream == 0xFF) break;  // D3DDECL_END
      const uint16_t offset = static_cast<uint16_t>(d0 & 0xFFFF);
      const uint32_t type = gmem::ReadU32(base, e + 4);
      const uint32_t muu = gmem::ReadU32(base, e + 8);   // Method|Usage|UsageIndex
      const uint32_t usage = (muu >> 16) & 0xFF, usageIdx = (muu >> 8) & 0xFF;
      REXGPU_INFO("[vtx]   s{} off={:>3} type=0x{:06X} {}{}", stream, offset, type,
                  UsageName(usage), usageIdx);
    }
  }
  __imp__XGSetVertexDeclaration(ctx, base);
}
