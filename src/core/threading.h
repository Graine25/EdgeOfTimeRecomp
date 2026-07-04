/**
 * @file    core/threading.h
 * @brief   Native threading, sleep, and frame timing hooks.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause - see LICENSE
 */
#pragma once

namespace eot {

void EnableHighResTimer();
void DisableHighResTimer();

void DemoteCurrentThreadToBackground();

[[noreturn]] void TerminateProcessNow(int exit_code = 0);

}
