/**
 * @file    installer/self_install.h
 * @brief   Copying the program into the install folder.
 *
 *          After reblue's installer/self_install (BSD 3-Clause, Tom Clay).
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <filesystem>
#include <string>

namespace eot::installer {

bool CopyProgramTo(const std::filesystem::path &install, std::string &error);

void SyncPortPackages(const std::filesystem::path &game);

void AdoptLegacyUserData(const std::filesystem::path &profile);

}
