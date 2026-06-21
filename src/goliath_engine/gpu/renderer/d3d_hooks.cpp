#include <rex/hook.h>
#include <rex/logging.h>

#include <bit>
#include <cstdint>

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
    eot::gpu::VideoSetClearColor(r, g, b, a);
    static int s_logged = 0;
    if (s_logged < 4) {
      ++s_logged;
      REXGPU_INFO("[clear] flags=0x{:X} color=({:.3f}, {:.3f}, {:.3f}, {:.3f})", flags, r, g, b, a);
    }
  }
}
