/**
 * @file    platform/process.h
 * @brief   Starting another copy of the program.
 *
 *          After reblue's platform/reboot (BSD 3-Clause, Tom Clay), reduced
 *          to what the installer needs: hand off to the executable it just
 *          copied into the install folder.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <filesystem>

namespace eot::platform {

std::filesystem::path ProgramDir();

bool SpawnReplacement(const std::filesystem::path &exe);

}
