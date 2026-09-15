/**
 * @file    core/sleep.cpp
 * @brief   Windows Sleep hook adapted from ReBlue's core/threading.cpp.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license BSD 3-Clause - see LICENSE
 */
#if defined(_WIN32)

#include <chrono>
#include <thread>

#include <rex/hook.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

REX_EXTERN(__imp__eot_Sleep);

REX_HOOK_RAW(eot_Sleep) {
  const uint32_t ms = ctx.r3.u32;
  if (ms == 0xFFFFFFFFu) {
    __imp__eot_Sleep(ctx, base);
    return;
  }

  if (ms == 0) {
    std::this_thread::yield();
  } else {
    const auto target =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    if (ms >= 2) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms) -
                                  std::chrono::microseconds(1500));
    } else {
      std::this_thread::yield();
    }
    while (std::chrono::steady_clock::now() < target) {
#if defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
#else
      std::this_thread::yield();
#endif
    }
  }
  ctx.r3.u64 = 0;
}

#endif
