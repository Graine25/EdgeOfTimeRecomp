/**
 * @file    core/app_root.cpp
 * @brief   Anchor directory for host-side app data (cache, logs, legacy
 * config).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "core/app_root.h"

#include <rex/filesystem.h>
#include <rex/platform/env.h>

namespace eot {

namespace {

#if defined(__APPLE__)
bool IsMacAppBundle() {
  const auto executable_dir = rex::filesystem::GetExecutableFolder();
  if (executable_dir.filename() != "MacOS")
    return false;
  const auto contents_dir = executable_dir.parent_path();
  return contents_dir.filename() == "Contents" &&
         contents_dir.parent_path().extension() == ".app";
}
#endif

}

bool IsPackagedApplication() {
#if !defined(_WIN32)
  if (rex::platform::env::get("APPIMAGE"))
    return true;
#if defined(__APPLE__)
  return IsMacAppBundle();
#endif
#endif
  return false;
}

std::filesystem::path UserConfigFolder() {
#if !defined(_WIN32)
  if (auto xdg = rex::platform::env::get("XDG_CONFIG_HOME");
      xdg && !xdg->empty())
    return std::filesystem::path(*xdg) / "reeot";
  if (auto home = rex::platform::env::get("HOME"); home && !home->empty())
    return std::filesystem::path(*home) / ".config" / "reeot";
#endif
  return {};
}

std::filesystem::path AppRootFolder() {
#if !defined(_WIN32)
  if (rex::platform::env::get("APPIMAGE")) {
    if (auto config = UserConfigFolder(); !config.empty())
      return config;
  }
#if defined(__APPLE__)
  if (IsMacAppBundle()) {
    if (auto home = rex::platform::env::get("HOME"); home && !home->empty())
      return std::filesystem::path(*home) / "Library" / "Application Support" /
             "reeot";
  }
#endif
#endif
  return rex::filesystem::GetExecutableFolder();
}

}
