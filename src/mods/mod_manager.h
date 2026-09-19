#pragma once

#include <filesystem>

namespace eot::mods {

inline constexpr const char *kModsFolderName = "mods";

void Initialize(const std::filesystem::path &install_root, const std::filesystem::path &game,
                const std::filesystem::path &profile);

}
