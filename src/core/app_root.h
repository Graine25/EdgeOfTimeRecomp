/**
 * @file    core/app_root.h
 * @brief   Anchor directory for host-side app data (cache, logs, legacy
 * config).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <filesystem>

namespace eot {

bool IsPackagedApplication();

std::filesystem::path AppRootFolder();

std::filesystem::path UserConfigFolder();

}
