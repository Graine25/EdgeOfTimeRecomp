/**
 * @file    core/sleep.cpp
 * @brief   Windows and macOS Sleep hook adapted from ReBlue's
 *          core/threading.cpp. The guest's job workers poll for work with
 *          Sleep(1) and the main thread waits on them several times a
 *          frame, so every millisecond the OS sleep overshoots is paid a
 *          few times over: the hybrid wait below sleeps most of the
 *          interval and spins the rest against the monotonic clock.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license BSD 3-Clause - see LICENSE
 */
#if defined(_WIN32) || defined(__APPLE__)

#include <chrono>
#include <thread>

#include <rex/hook.h>

#include "core/cpu.h"

REX_EXTERN(__imp__eot_Sleep);

REX_HOOK_RAW(eot_Sleep) {
  const uint32_t ms = ctx.r3.u32;
  if (ms == 0xFFFFFFFFu) {
    __imp__eot_Sleep(ctx, base);
    return;
  }

#if defined(__APPLE__)
  constexpr std::chrono::microseconds kSleepShortfall(2500);
  constexpr uint32_t kSleepFromMs = 4;
#else
  constexpr std::chrono::microseconds kSleepShortfall(1500);
  constexpr uint32_t kSleepFromMs = 2;
#endif
  if (ms == 0) {
    std::this_thread::yield();
  } else {
    const auto target =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    if (ms >= kSleepFromMs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms) - kSleepShortfall);
    } else {
      std::this_thread::yield();
    }
    while (std::chrono::steady_clock::now() < target)
      eot::cpu::Relax();
  }
  ctx.r3.u64 = 0;
}

#endif
