#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace eot::text {

bool LoadTranslation(const std::filesystem::path &game, std::string_view language);

size_t TranslatedLines();

}
