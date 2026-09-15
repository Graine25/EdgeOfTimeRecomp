#include "platform/language.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace eot::platform {

uint32_t SystemXLanguage() {
#if defined(_WIN32)
  switch (PRIMARYLANGID(::GetUserDefaultUILanguage())) {
  case LANG_FRENCH:
    return kXLanguageFrench;
  case LANG_ITALIAN:
    return kXLanguageItalian;
  case LANG_GERMAN:
    return kXLanguageGerman;
  case LANG_SPANISH:
    return kXLanguageSpanish;
  default:
    return kXLanguageEnglish;
  }
#else
  return kXLanguageEnglish;
#endif
}

uint32_t XLanguageFor(std::string_view eot_language) {
  if (eot_language == "fr")
    return kXLanguageFrench;
  if (eot_language == "it")
    return kXLanguageItalian;
  if (eot_language == "de")
    return kXLanguageGerman;
  if (eot_language == "es")
    return kXLanguageSpanish;
  if (eot_language == "en")
    return kXLanguageEnglish;
  return SystemXLanguage();
}

std::string SystemLanguageTag() {
#if defined(_WIN32)
  wchar_t name[16] = {};
  if (::GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SISO639LANGNAME, name, 16) > 1) {
    std::string tag;
    for (const wchar_t *p = name; *p; ++p)
      tag.push_back(static_cast<char>(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p));
    return tag;
  }
#endif
  return "en";
}

std::string TranslationTag(std::string_view eot_language) {
  return eot_language == "auto" ? SystemLanguageTag() : std::string(eot_language);
}

const char *XLanguageName(uint32_t xlanguage) {
  switch (xlanguage) {
  case kXLanguageFrench:
    return "French";
  case kXLanguageItalian:
    return "Italian";
  case kXLanguageGerman:
    return "German";
  case kXLanguageSpanish:
    return "Spanish";
  default:
    return "English";
  }
}

}
