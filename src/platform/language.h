#pragma once

#include <cstdint>
#include <string_view>

namespace eot::platform {

constexpr uint32_t kXLanguageEnglish = 1;
constexpr uint32_t kXLanguageGerman = 3;
constexpr uint32_t kXLanguageFrench = 4;
constexpr uint32_t kXLanguageSpanish = 5;
constexpr uint32_t kXLanguageItalian = 6;

uint32_t XLanguageFor(std::string_view eot_language);

uint32_t SystemXLanguage();

const char *XLanguageName(uint32_t xlanguage);

}
