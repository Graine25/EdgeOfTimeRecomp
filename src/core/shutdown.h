/**
 * @file    core/shutdown.h
 * @brief   The one ordered exit path: quiesce the guest, drain the GPU, flush,
 *          then kill the process.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <functional>

namespace eot {

enum class ShutdownReason {
  WindowClose,
  GuestExit,
  Fatal,
  InitFailure,
};

void SetShutdownDispatcher(std::function<bool(std::function<void()>)> dispatch);

void SetShutdownUIPump(std::function<void()> pump);

void RequestShutdown(ShutdownReason reason, int exit_code = 0);

bool IsShuttingDown();

void QuiesceForExit();

}
