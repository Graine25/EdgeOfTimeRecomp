#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace eot::text {

bool LoadTranslation(const std::filesystem::path &game, std::string_view language);

void PackageMounted(uint32_t id, uint32_t package);

size_t TranslatedLines();

}
