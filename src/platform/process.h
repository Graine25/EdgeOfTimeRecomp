/**
 * @file    platform/process.h
 * @brief   This process and other copies of it: the single-instance lock,
 *          starting another copy, raising the window.
 *
 *          After the reblue platform/reboot (BSD 3-Clause, Tom Clay),
 *          reduced to what the installer and the startup need.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <filesystem>

namespace eot::platform {

#if defined(_WIN32)
constexpr const char *kExecutableFileName = "reeot.exe";
#else
constexpr const char *kExecutableFileName = "reeot";
#endif

std::filesystem::path ProgramDir();

std::filesystem::path LaunchPath();

bool AcquireInstanceLock();

bool SpawnReplacement(const std::filesystem::path &exe);

bool RelaunchSelf();

void RaiseMainWindow();

}
